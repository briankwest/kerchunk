/*
 * kerchunk-reflectd — central reflector for kerchunk repeater linking.
 *
 * Phase 1 scope: plain TCP listener (no TLS, no SRTP, no audio).
 * Speaks line-delimited JSON over TCP. Implements the auth and
 * keepalive subset of the protocol so a probe client can complete a
 * full hello → login → ping cycle and validate the wire shapes from
 * include/kerchunk_link_proto.h.
 *
 * Talkgroups, audio bridging, floor control, the full message catalog,
 * TLS and SRTP all land in subsequent phases (see PLAN-LINK.md § 10).
 *
 * One node is hard-coded for now via -n / -k flags — phase 2 reads the
 * roster from /etc/kerchunk-reflectd.conf.
 */

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "../../vendor/mongoose.h"
#include "../../include/kerchunk_link_proto.h"

/* ── Globals ───────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop = 0;

/* Phase 1: one hard-coded node. Replaced by an INI-driven roster in
 * phase 2. node_id is the operator-chosen identifier; psk is the 32-byte
 * preshared key that both the reflector and the local mod_link must
 * agree on. */
static struct {
    char     node_id[64];
    uint8_t  psk[KERCHUNK_LINK_PSK_BYTES];
    bool     in_use;
} g_node;

static int g_listen_port = KERCHUNK_LINK_DEFAULT_WS_PORT;

/* ── Per-connection state ──────────────────────────────────────────── */

typedef enum {
    CST_AWAITING_LOGIN,   /* hello sent, waiting for login */
    CST_AUTHENTICATED,    /* login_ok delivered, ping/pong + future audio */
} conn_state_t;

typedef struct {
    conn_state_t state;
    uint8_t      challenge[KERCHUNK_LINK_CHALLENGE_BYTES];
    char         node_id[64];           /* set after auth */
    time_t       last_traffic;
} conn_t;

/* ── Hex helpers ───────────────────────────────────────────────────── */

static void bytes_to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2*i]     = d[in[i] >> 4];
        out[2*i + 1] = d[in[i] & 0x0f];
    }
    out[2*n] = '\0';
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char hi = hex[2*i], lo = hex[2*i + 1];
        if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo))
            return -1;
        unsigned v;
        if (sscanf(hex + 2*i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* ── JSON send helpers ─────────────────────────────────────────────── */

static void send_json(struct mg_connection *c, const char *json)
{
    mg_send(c, json, strlen(json));
    mg_send(c, "\n", 1);
}

static void send_error_close(struct mg_connection *c, const char *type,
                             const char *code, const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"code\":\"%s\",\"msg\":\"%s\"}",
             type, code, msg);
    send_json(c, buf);
    c->is_draining = 1;   /* flush then close */
}

/* ── Connection lifecycle ──────────────────────────────────────────── */

static void send_hello(struct mg_connection *c, conn_t *cs)
{
    if (RAND_bytes(cs->challenge, sizeof(cs->challenge)) != 1) {
        send_error_close(c, LINK_MSG_ERROR, LINK_ERR_INTERNAL,
                         "RAND_bytes failed");
        return;
    }
    char chal_hex[KERCHUNK_LINK_CHALLENGE_BYTES * 2 + 1];
    bytes_to_hex(cs->challenge, sizeof(cs->challenge), chal_hex);

    char buf[KERCHUNK_LINK_MAX_MSG];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"challenge\":\"%s\","
             "\"reflector_version\":\"reflectd %s\","
             "\"min_client_version\":\"kerchunk 1.0.0\","
             "\"proto\":\"%s\"}",
             LINK_MSG_HELLO, chal_hex, "0.1.0",
             KERCHUNK_LINK_PROTO_VERSION);
    send_json(c, buf);
    cs->state = CST_AWAITING_LOGIN;
    cs->last_traffic = time(NULL);
}

/* HMAC-SHA256(psk, challenge || nonce). Returns 0 on match. */
static int verify_login_hmac(const uint8_t *psk, size_t psk_len,
                             const uint8_t *challenge,
                             const uint8_t *nonce,
                             const uint8_t *claimed)
{
    uint8_t input[KERCHUNK_LINK_CHALLENGE_BYTES + KERCHUNK_LINK_NONCE_BYTES];
    memcpy(input,                                challenge,
           KERCHUNK_LINK_CHALLENGE_BYTES);
    memcpy(input + KERCHUNK_LINK_CHALLENGE_BYTES, nonce,
           KERCHUNK_LINK_NONCE_BYTES);

    uint8_t  computed[KERCHUNK_LINK_HMAC_BYTES];
    unsigned out_len = sizeof(computed);
    if (!HMAC(EVP_sha256(), psk, (int)psk_len,
              input, sizeof(input),
              computed, &out_len))
        return -1;
    if (out_len != KERCHUNK_LINK_HMAC_BYTES) return -1;

    /* Constant-time compare. */
    unsigned char diff = 0;
    for (size_t i = 0; i < KERCHUNK_LINK_HMAC_BYTES; i++)
        diff |= computed[i] ^ claimed[i];
    return diff == 0 ? 0 : -1;
}

static void handle_login(struct mg_connection *c, conn_t *cs,
                         struct mg_str body)
{
    char *node_id     = mg_json_get_str(body, "$.node_id");
    char *key_hmac    = mg_json_get_str(body, "$.key_hmac");
    char *nonce_hex   = mg_json_get_str(body, "$.nonce");
    char *client_ver  = mg_json_get_str(body, "$.client_version");

    if (!node_id || !key_hmac || !nonce_hex) {
        send_error_close(c, LINK_MSG_LOGIN_DENIED,
                         LINK_ERR_PROTOCOL_ERROR,
                         "login missing required fields");
        goto out;
    }

    /* Phase 1 has one hard-coded node. */
    if (!g_node.in_use || strcmp(node_id, g_node.node_id) != 0) {
        send_error_close(c, LINK_MSG_LOGIN_DENIED,
                         LINK_ERR_UNKNOWN_NODE, "no such node");
        goto out;
    }

    uint8_t nonce[KERCHUNK_LINK_NONCE_BYTES];
    uint8_t claimed[KERCHUNK_LINK_HMAC_BYTES];
    if (strlen(nonce_hex) != 2 * KERCHUNK_LINK_NONCE_BYTES ||
        hex_to_bytes(nonce_hex, nonce, sizeof(nonce)) != 0) {
        send_error_close(c, LINK_MSG_LOGIN_DENIED,
                         LINK_ERR_PROTOCOL_ERROR, "bad nonce");
        goto out;
    }
    if (strlen(key_hmac) != 2 * KERCHUNK_LINK_HMAC_BYTES ||
        hex_to_bytes(key_hmac, claimed, sizeof(claimed)) != 0) {
        send_error_close(c, LINK_MSG_LOGIN_DENIED,
                         LINK_ERR_PROTOCOL_ERROR, "bad hmac");
        goto out;
    }

    if (verify_login_hmac(g_node.psk, sizeof(g_node.psk),
                          cs->challenge, nonce, claimed) != 0) {
        send_error_close(c, LINK_MSG_LOGIN_DENIED,
                         LINK_ERR_BAD_KEY, "HMAC mismatch");
        goto out;
    }

    /* Mark authenticated. SRTP material + TG assignment land in phase 3. */
    cs->state = CST_AUTHENTICATED;
    snprintf(cs->node_id, sizeof(cs->node_id), "%s", node_id);

    char buf[KERCHUNK_LINK_MAX_MSG];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"node_id\":\"%s\","
             "\"keepalive_s\":%d,\"hangtime_ms\":%d,"
             "\"proto\":\"%s\"}",
             LINK_MSG_LOGIN_OK, node_id,
             KERCHUNK_LINK_DEFAULT_KEEPALIVE_S,
             KERCHUNK_LINK_DEFAULT_HANGTIME_MS,
             KERCHUNK_LINK_PROTO_VERSION);
    send_json(c, buf);

    fprintf(stderr, "reflectd: node %s logged in (client %s)\n",
            node_id, client_ver ? client_ver : "?");

out:
    free(node_id);
    free(key_hmac);
    free(nonce_hex);
    free(client_ver);
}

static void handle_ping(struct mg_connection *c, struct mg_str body)
{
    long seq = mg_json_get_long(body, "$.seq", 0);
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"seq\":%ld}", LINK_MSG_PONG, seq);
    send_json(c, buf);
}

/* Dispatch one complete JSON message. */
static void handle_message(struct mg_connection *c, conn_t *cs,
                           const char *line, size_t len)
{
    cs->last_traffic = time(NULL);

    struct mg_str body = mg_str_n(line, len);
    char *type = mg_json_get_str(body, "$.type");
    if (!type) {
        send_error_close(c, LINK_MSG_ERROR, LINK_ERR_PROTOCOL_ERROR,
                         "no type");
        return;
    }

    if (strcmp(type, LINK_MSG_LOGIN) == 0) {
        if (cs->state != CST_AWAITING_LOGIN) {
            send_error_close(c, LINK_MSG_ERROR, LINK_ERR_PROTOCOL_ERROR,
                             "duplicate login");
        } else {
            handle_login(c, cs, body);
        }
    } else if (cs->state != CST_AUTHENTICATED) {
        send_error_close(c, LINK_MSG_ERROR, LINK_ERR_PROTOCOL_ERROR,
                         "not authenticated");
    } else if (strcmp(type, LINK_MSG_PING) == 0) {
        handle_ping(c, body);
    } else {
        /* Phase 1 only knows login + ping. Anything else is a no-op
         * for now (logged so phase 2+ work surfaces gaps). */
        fprintf(stderr, "reflectd: unhandled message type '%s' (phase 1)\n",
                type);
    }

    free(type);
}

/* Pull line-delimited JSON out of c->recv, dispatch each. */
static void drain_lines(struct mg_connection *c, conn_t *cs)
{
    while (cs && cs->state != CST_AWAITING_LOGIN + 100 /* never */) {
        char *nl = memchr(c->recv.buf, '\n', c->recv.len);
        if (!nl) {
            /* No complete line yet. Guard against ridiculous lines. */
            if (c->recv.len > KERCHUNK_LINK_MAX_MSG) {
                send_error_close(c, LINK_MSG_ERROR, LINK_ERR_PROTOCOL_ERROR,
                                 "message too long");
                mg_iobuf_del(&c->recv, 0, c->recv.len);
            }
            return;
        }
        size_t line_len = (size_t)(nl - (char *)c->recv.buf);
        if (line_len > 0) {
            /* Skip empty lines (allow CRLF tolerance: trim trailing CR). */
            char *line = (char *)c->recv.buf;
            size_t l   = line_len;
            if (l && line[l - 1] == '\r') l--;
            if (l) handle_message(c, cs, line, l);
        }
        mg_iobuf_del(&c->recv, 0, line_len + 1);
        if (c->is_closing || c->is_draining) return;
    }
}

/* ── Mongoose event handler ────────────────────────────────────────── */

static void evh(struct mg_connection *c, int ev, void *ev_data)
{
    (void)ev_data;

    if (ev == MG_EV_OPEN && c->is_listening) {
        fprintf(stderr, "reflectd: listening on tcp://0.0.0.0:%d\n",
                g_listen_port);
        return;
    }
    if (ev == MG_EV_ACCEPT) {
        conn_t *cs = calloc(1, sizeof(*cs));
        if (!cs) { c->is_closing = 1; return; }
        c->fn_data = cs;
        send_hello(c, cs);
        return;
    }
    if (ev == MG_EV_READ) {
        conn_t *cs = (conn_t *)c->fn_data;
        if (cs) drain_lines(c, cs);
        return;
    }
    if (ev == MG_EV_CLOSE) {
        conn_t *cs = (conn_t *)c->fn_data;
        if (cs && cs->state == CST_AUTHENTICATED) {
            fprintf(stderr, "reflectd: node %s disconnected\n", cs->node_id);
        }
        free(cs);
        c->fn_data = NULL;
        return;
    }
}

/* ── Signal + main ─────────────────────────────────────────────────── */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s -n <node_id> -k <psk_hex> [-p <port>]\n"
        "  -n   Node id permitted to connect (phase 1 supports one)\n"
        "  -k   Preshared key as %d-byte (= %d hex char) hex string\n"
        "  -p   TCP listen port (default %d)\n",
        argv0, KERCHUNK_LINK_PSK_BYTES, KERCHUNK_LINK_PSK_BYTES * 2,
        KERCHUNK_LINK_DEFAULT_WS_PORT);
}

int main(int argc, char **argv)
{
    const char *node_id = NULL;
    const char *psk_hex = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "n:k:p:h")) != -1) {
        switch (opt) {
        case 'n': node_id = optarg; break;
        case 'k': psk_hex = optarg; break;
        case 'p': g_listen_port = atoi(optarg); break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (!node_id || !psk_hex) { usage(argv[0]); return 2; }
    if (strlen(psk_hex) != 2 * KERCHUNK_LINK_PSK_BYTES) {
        fprintf(stderr, "reflectd: psk must be %d hex chars\n",
                KERCHUNK_LINK_PSK_BYTES * 2);
        return 2;
    }
    snprintf(g_node.node_id, sizeof(g_node.node_id), "%s", node_id);
    if (hex_to_bytes(psk_hex, g_node.psk, KERCHUNK_LINK_PSK_BYTES) != 0) {
        fprintf(stderr, "reflectd: psk is not valid hex\n");
        return 2;
    }
    g_node.in_use = true;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct mg_mgr mgr;
    mg_log_set(MG_LL_ERROR);   /* must precede mg_mgr_init or it logs init banner */
    mg_mgr_init(&mgr);

    char url[64];
    snprintf(url, sizeof(url), "tcp://0.0.0.0:%d", g_listen_port);
    if (!mg_listen(&mgr, url, evh, NULL)) {
        fprintf(stderr, "reflectd: listen on %s failed\n", url);
        mg_mgr_free(&mgr);
        return 1;
    }

    while (!g_stop)
        mg_mgr_poll(&mgr, 100);

    fprintf(stderr, "reflectd: shutting down\n");
    mg_mgr_free(&mgr);
    return 0;
}
