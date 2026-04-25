/*
 * kerchunk-link-probe — phase-1 protocol smoke test for kerchunk-reflectd.
 *
 * Connects via plain TCP, completes the hello → login → ping cycle, and
 * exits 0 on success, non-zero on protocol failure. Used to validate the
 * wire shapes from include/kerchunk_link_proto.h before the real mod_link
 * exists. Phase 5 retires this in favor of integration tests that drive
 * mod_link end-to-end.
 */

#include <ctype.h>
#include <getopt.h>
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

/* ── Probe state ───────────────────────────────────────────────────── */

typedef enum {
    PST_CONNECTING,
    PST_AWAITING_HELLO,
    PST_AWAITING_LOGIN_OK,
    PST_PINGING,
    PST_DONE,
    PST_FAILED,
} probe_state_t;

static struct {
    probe_state_t state;
    const char   *node_id;
    uint8_t       psk[KERCHUNK_LINK_PSK_BYTES];
    int           pings_sent;
    int           pongs_recv;
    int           pings_target;
    char          fail_reason[256];
} g;

static void send_json(struct mg_connection *c, const char *json)
{
    mg_send(c, json, strlen(json));
    mg_send(c, "\n", 1);
}

static void fail(struct mg_connection *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.fail_reason, sizeof(g.fail_reason), fmt, ap);
    va_end(ap);
    g.state = PST_FAILED;
    c->is_closing = 1;
}

/* ── Protocol steps ────────────────────────────────────────────────── */

static void on_hello(struct mg_connection *c, struct mg_str body)
{
    char *chal_hex = mg_json_get_str(body, "$.challenge");
    if (!chal_hex || strlen(chal_hex) != 2 * KERCHUNK_LINK_CHALLENGE_BYTES) {
        fail(c, "hello missing/short challenge");
        free(chal_hex);
        return;
    }
    uint8_t challenge[KERCHUNK_LINK_CHALLENGE_BYTES];
    if (hex_to_bytes(chal_hex, challenge, sizeof(challenge)) != 0) {
        fail(c, "hello challenge not hex");
        free(chal_hex);
        return;
    }
    free(chal_hex);

    /* Build login: nonce + HMAC-SHA256(psk, challenge || nonce). */
    uint8_t nonce[KERCHUNK_LINK_NONCE_BYTES];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
        fail(c, "RAND_bytes failed");
        return;
    }
    uint8_t input[KERCHUNK_LINK_CHALLENGE_BYTES + KERCHUNK_LINK_NONCE_BYTES];
    memcpy(input,                                challenge, sizeof(challenge));
    memcpy(input + KERCHUNK_LINK_CHALLENGE_BYTES, nonce,    sizeof(nonce));
    uint8_t  mac[KERCHUNK_LINK_HMAC_BYTES];
    unsigned mac_len = sizeof(mac);
    if (!HMAC(EVP_sha256(), g.psk, sizeof(g.psk),
              input, sizeof(input), mac, &mac_len) ||
        mac_len != KERCHUNK_LINK_HMAC_BYTES) {
        fail(c, "HMAC failed");
        return;
    }

    char nonce_hex[KERCHUNK_LINK_NONCE_BYTES * 2 + 1];
    char mac_hex[KERCHUNK_LINK_HMAC_BYTES   * 2 + 1];
    bytes_to_hex(nonce, sizeof(nonce), nonce_hex);
    bytes_to_hex(mac,   sizeof(mac),   mac_hex);

    char buf[KERCHUNK_LINK_MAX_MSG];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"node_id\":\"%s\","
             "\"key_hmac\":\"%s\",\"nonce\":\"%s\","
             "\"client_version\":\"link-probe 0.1\"}",
             LINK_MSG_LOGIN, g.node_id, mac_hex, nonce_hex);
    send_json(c, buf);
    g.state = PST_AWAITING_LOGIN_OK;
}

static void on_login_ok(struct mg_connection *c)
{
    /* Kick off the ping cadence. */
    g.state = PST_PINGING;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"seq\":%d}",
             LINK_MSG_PING, ++g.pings_sent);
    send_json(c, buf);
}

static void on_pong(struct mg_connection *c, struct mg_str body)
{
    long seq = mg_json_get_long(body, "$.seq", -1);
    if (seq != g.pings_sent) {
        fail(c, "pong seq %ld != expected %d", seq, g.pings_sent);
        return;
    }
    g.pongs_recv++;
    if (g.pongs_recv >= g.pings_target) {
        g.state = PST_DONE;
        c->is_draining = 1;
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"seq\":%d}",
             LINK_MSG_PING, ++g.pings_sent);
    send_json(c, buf);
}

static void handle_message(struct mg_connection *c, const char *line, size_t len)
{
    struct mg_str body = mg_str_n(line, len);
    char *type = mg_json_get_str(body, "$.type");
    if (!type) { fail(c, "msg missing type: %.*s", (int)len, line); return; }

    if (strcmp(type, LINK_MSG_HELLO) == 0 &&
        g.state == PST_AWAITING_HELLO) {
        on_hello(c, body);
    } else if (strcmp(type, LINK_MSG_LOGIN_OK) == 0 &&
               g.state == PST_AWAITING_LOGIN_OK) {
        on_login_ok(c);
    } else if (strcmp(type, LINK_MSG_PONG) == 0 &&
               g.state == PST_PINGING) {
        on_pong(c, body);
    } else if (strcmp(type, LINK_MSG_LOGIN_DENIED) == 0) {
        char *code = mg_json_get_str(body, "$.code");
        char *msg  = mg_json_get_str(body, "$.msg");
        fail(c, "login_denied(%s): %s", code ? code : "?", msg ? msg : "");
        free(code); free(msg);
    } else if (strcmp(type, LINK_MSG_ERROR) == 0) {
        char *code = mg_json_get_str(body, "$.code");
        char *msg  = mg_json_get_str(body, "$.msg");
        fail(c, "error(%s): %s", code ? code : "?", msg ? msg : "");
        free(code); free(msg);
    } else {
        fail(c, "unexpected '%s' in state %d", type, g.state);
    }

    free(type);
}

static void drain_lines(struct mg_connection *c)
{
    while (1) {
        char *nl = memchr(c->recv.buf, '\n', c->recv.len);
        if (!nl) {
            if (c->recv.len > KERCHUNK_LINK_MAX_MSG) {
                fail(c, "incoming line > %d bytes", KERCHUNK_LINK_MAX_MSG);
                mg_iobuf_del(&c->recv, 0, c->recv.len);
            }
            return;
        }
        size_t line_len = (size_t)(nl - (char *)c->recv.buf);
        char  *line     = (char *)c->recv.buf;
        size_t l        = line_len;
        if (l && line[l - 1] == '\r') l--;
        if (l) handle_message(c, line, l);
        mg_iobuf_del(&c->recv, 0, line_len + 1);
        if (c->is_closing || c->is_draining) return;
    }
}

static void evh(struct mg_connection *c, int ev, void *ev_data)
{
    (void)ev_data;
    if (ev == MG_EV_CONNECT) {
        g.state = PST_AWAITING_HELLO;
    } else if (ev == MG_EV_READ) {
        drain_lines(c);
    } else if (ev == MG_EV_ERROR) {
        fail(c, "connect/io error: %s",
             ev_data ? (char *)ev_data : "(unknown)");
    } else if (ev == MG_EV_CLOSE) {
        if (g.state != PST_DONE && g.state != PST_FAILED)
            fail(c, "connection closed in state %d", g.state);
    }
}

/* ── main ──────────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s -n <node_id> -k <psk_hex> [-u tcp://host:port] [-c N]\n"
        "  -n   Node id to log in as\n"
        "  -k   Preshared key, %d hex chars\n"
        "  -u   Reflector URL (default tcp://127.0.0.1:%d)\n"
        "  -c   Number of ping/pong cycles (default 3)\n",
        argv0, KERCHUNK_LINK_PSK_BYTES * 2,
        KERCHUNK_LINK_DEFAULT_WS_PORT);
}

int main(int argc, char **argv)
{
    const char *psk_hex = NULL;
    char        url[128];
    snprintf(url, sizeof(url), "tcp://127.0.0.1:%d",
             KERCHUNK_LINK_DEFAULT_WS_PORT);

    g.pings_target = 3;
    g.state        = PST_CONNECTING;

    int opt;
    while ((opt = getopt(argc, argv, "n:k:u:c:h")) != -1) {
        switch (opt) {
        case 'n': g.node_id = optarg; break;
        case 'k': psk_hex = optarg; break;
        case 'u': snprintf(url, sizeof(url), "%s", optarg); break;
        case 'c': g.pings_target = atoi(optarg); break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (!g.node_id || !psk_hex) { usage(argv[0]); return 2; }
    if (strlen(psk_hex) != 2 * KERCHUNK_LINK_PSK_BYTES ||
        hex_to_bytes(psk_hex, g.psk, sizeof(g.psk)) != 0) {
        fprintf(stderr, "probe: psk must be %d hex chars\n",
                KERCHUNK_LINK_PSK_BYTES * 2);
        return 2;
    }

    struct mg_mgr mgr;
    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&mgr);
    if (!mg_connect(&mgr, url, evh, NULL)) {
        fprintf(stderr, "probe: connect to %s failed\n", url);
        mg_mgr_free(&mgr);
        return 1;
    }

    /* Hard 5 s overall timeout — phase 1 is supposed to be near-instant. */
    time_t deadline = time(NULL) + 5;
    while (g.state != PST_DONE && g.state != PST_FAILED &&
           time(NULL) < deadline) {
        mg_mgr_poll(&mgr, 50);
    }
    if (g.state != PST_DONE && g.state != PST_FAILED)
        snprintf(g.fail_reason, sizeof(g.fail_reason),
                 "timeout in state %d after 5s", g.state);

    mg_mgr_free(&mgr);

    if (g.state == PST_DONE) {
        printf("OK — login + %d ping/pong cycles complete\n", g.pongs_recv);
        return 0;
    }
    fprintf(stderr, "FAIL — %s\n", g.fail_reason);
    return 1;
}
