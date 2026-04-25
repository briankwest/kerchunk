/*
 * kerchunk-link-probe — phase-2 protocol smoke test for kerchunk-reflectd.
 *
 * Connects via WebSocket (ws:// or wss://), completes login, optionally
 * switches talkgroup, and optionally drives the floor with debug
 * simulate_talk_start / simulate_talk_end messages so floor / talker /
 * floor_denied flows can be exercised end-to-end without audio.
 *
 * Phase 5 retires this in favor of mod_link integration tests.
 */

#include <ctype.h>
#include <getopt.h>
#include <stdarg.h>
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

/* ── Hex helpers ──────────────────────────────────────────────────── */

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

/* ── Probe state ──────────────────────────────────────────────────── */

typedef enum {
    PST_CONNECTING,
    PST_AWAITING_HELLO,
    PST_AWAITING_LOGIN_OK,
    PST_AWAITING_TG_OK,
    PST_PINGING,
    PST_TALKING,
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
    int           switch_tg;          /* >0: send set_tg after login */
    int           talk_then_exit;     /* 1 = send simulate_talk_start, exit on talker echo */
    int           hold_ms;            /* >0: hold the floor this long before releasing */
    int64_t       hold_until_ms;
    int           talker_seen;
    int           floor_denied_seen;
    int           tg_denied_seen;
    char          fail_reason[256];
} g;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void send_json(struct mg_connection *c, const char *json)
{
    mg_ws_send(c, json, strlen(json), WEBSOCKET_OP_TEXT);
}

static void fail(struct mg_connection *c, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g.fail_reason, sizeof(g.fail_reason), fmt, ap);
    va_end(ap);
    g.state = PST_FAILED;
    if (c) c->is_closing = 1;
}

/* ── Protocol steps ───────────────────────────────────────────────── */

static void on_hello(struct mg_connection *c, struct mg_str body)
{
    char *chal_hex = mg_json_get_str(body, "$.challenge");
    if (!chal_hex || strlen(chal_hex) != 2 * KERCHUNK_LINK_CHALLENGE_BYTES) {
        fail(c, "hello missing/short challenge");
        free(chal_hex); return;
    }
    uint8_t challenge[KERCHUNK_LINK_CHALLENGE_BYTES];
    if (hex_to_bytes(chal_hex, challenge, sizeof(challenge)) != 0) {
        fail(c, "hello challenge not hex");
        free(chal_hex); return;
    }
    free(chal_hex);

    uint8_t nonce[KERCHUNK_LINK_NONCE_BYTES];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
        fail(c, "RAND_bytes failed"); return;
    }
    uint8_t input[KERCHUNK_LINK_CHALLENGE_BYTES + KERCHUNK_LINK_NONCE_BYTES];
    memcpy(input,                                challenge, sizeof(challenge));
    memcpy(input + KERCHUNK_LINK_CHALLENGE_BYTES, nonce,    sizeof(nonce));
    uint8_t  mac[KERCHUNK_LINK_HMAC_BYTES];
    unsigned mac_len = sizeof(mac);
    if (!HMAC(EVP_sha256(), g.psk, sizeof(g.psk),
              input, sizeof(input), mac, &mac_len) ||
        mac_len != KERCHUNK_LINK_HMAC_BYTES) {
        fail(c, "HMAC failed"); return;
    }

    char nonce_hex[KERCHUNK_LINK_NONCE_BYTES * 2 + 1];
    char mac_hex[KERCHUNK_LINK_HMAC_BYTES   * 2 + 1];
    bytes_to_hex(nonce, sizeof(nonce), nonce_hex);
    bytes_to_hex(mac,   sizeof(mac),   mac_hex);

    char buf[KERCHUNK_LINK_MAX_MSG];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"node_id\":\"%s\","
             "\"key_hmac\":\"%s\",\"nonce\":\"%s\","
             "\"client_version\":\"link-probe 0.2\"}",
             LINK_MSG_LOGIN, g.node_id, mac_hex, nonce_hex);
    send_json(c, buf);
    g.state = PST_AWAITING_LOGIN_OK;
}

static void send_first_ping(struct mg_connection *c)
{
    g.state = PST_PINGING;
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"seq\":%d}",
             LINK_MSG_PING, ++g.pings_sent);
    send_json(c, buf);
}

static void on_login_ok(struct mg_connection *c)
{
    if (g.switch_tg > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"tg\":%d}",
                 LINK_MSG_SET_TG, g.switch_tg);
        send_json(c, buf);
        g.state = PST_AWAITING_TG_OK;
    } else if (g.talk_then_exit) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"type\":\"simulate_talk_start\"}");
        send_json(c, buf);
        g.state = PST_TALKING;
    } else {
        send_first_ping(c);
    }
}

static void on_tg_ok(struct mg_connection *c)
{
    if (g.talk_then_exit) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"type\":\"simulate_talk_start\"}");
        send_json(c, buf);
        g.state = PST_TALKING;
    } else {
        send_first_ping(c);
    }
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
    if (!type) { fail(c, "no type field"); return; }

    if (strcmp(type, LINK_MSG_HELLO) == 0 &&
        g.state == PST_AWAITING_HELLO) {
        on_hello(c, body);
    } else if (strcmp(type, LINK_MSG_LOGIN_OK) == 0 &&
               g.state == PST_AWAITING_LOGIN_OK) {
        on_login_ok(c);
    } else if (strcmp(type, LINK_MSG_TG_OK) == 0 &&
               g.state == PST_AWAITING_TG_OK) {
        on_tg_ok(c);
    } else if (strcmp(type, LINK_MSG_TG_DENIED) == 0) {
        char *code = mg_json_get_str(body, "$.code");
        printf("tg_denied: code=%s\n", code ? code : "?");
        g.tg_denied_seen = 1;
        free(code);
        g.state = PST_DONE;
        c->is_draining = 1;
    } else if (strcmp(type, LINK_MSG_TALKER) == 0) {
        char *who = mg_json_get_str(body, "$.node_id");
        long  tg  = mg_json_get_long(body, "$.tg", 0);
        printf("talker: tg=%ld node=%s\n", tg, who ? who : "(idle)");
        if (who && g.talk_then_exit && strcmp(who, g.node_id) == 0)
            g.talker_seen = 1;
        free(who);
        if (g.talker_seen && g.state == PST_TALKING && g.hold_ms == 0) {
            /* End our simulated transmission and wait for the idle
             * `talker(null)` echo to confirm release before exiting. */
            char buf[64];
            snprintf(buf, sizeof(buf), "{\"type\":\"simulate_talk_end\"}");
            send_json(c, buf);
            g.state = PST_DONE;
            c->is_draining = 1;
        } else if (g.talker_seen && g.state == PST_TALKING && g.hold_ms > 0
                   && g.hold_until_ms == 0) {
            g.hold_until_ms = now_ms() + g.hold_ms;
        }
    } else if (strcmp(type, LINK_MSG_FLOOR_DENIED) == 0) {
        char *cur = mg_json_get_str(body, "$.current_talker");
        printf("floor_denied: current_talker=%s\n", cur ? cur : "?");
        g.floor_denied_seen = 1;
        free(cur);
        g.state = PST_DONE;
        c->is_draining = 1;
    } else if (strcmp(type, LINK_MSG_FLOOR_REVOKED) == 0) {
        char *code = mg_json_get_str(body, "$.code");
        printf("floor_revoked: code=%s\n", code ? code : "?");
        free(code);
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
        /* Unhandled in this state — log and continue (e.g. talker echoes
         * for other nodes during multi-client tests). */
        printf("(ignored '%s' in state %d)\n", type, g.state);
    }

    free(type);
}

static void evh(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_CONNECT) {
        /* For wss://, mongoose handles TLS; we still need to wait for
         * MG_EV_WS_OPEN before sending. */
    } else if (ev == MG_EV_WS_OPEN) {
        g.state = PST_AWAITING_HELLO;
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        handle_message(c, wm->data.buf, wm->data.len);
    } else if (ev == MG_EV_ERROR) {
        fail(c, "io: %s", ev_data ? (char *)ev_data : "(unknown)");
    } else if (ev == MG_EV_CLOSE) {
        if (g.state != PST_DONE && g.state != PST_FAILED)
            fail(NULL, "connection closed in state %d", g.state);
    }
}

/* ── main ─────────────────────────────────────────────────────────── */

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s -n <node_id> -k <psk_hex> [-u <ws-url>] [-c N]\n"
        "       [--tg N] [--talk] [-T <secs>]\n"
        "  -n        Node id to log in as\n"
        "  -k        Preshared key, %d hex chars\n"
        "  -u        Reflector URL (default ws://127.0.0.1:%d/link)\n"
        "  -c        Ping/pong cycle count (default 3; 0 to skip)\n"
        "  --tg N    Send set_tg N after login (skips ping if --talk also set)\n"
        "  --talk    Send simulate_talk_start, exit on own-talker echo\n"
        "  -T N      Overall timeout seconds (default 5)\n",
        argv0, KERCHUNK_LINK_PSK_BYTES * 2,
        KERCHUNK_LINK_DEFAULT_WS_PORT);
}

int main(int argc, char **argv)
{
    const char *psk_hex = NULL;
    char        url[160];
    snprintf(url, sizeof(url), "ws://127.0.0.1:%d/link",
             KERCHUNK_LINK_DEFAULT_WS_PORT);

    g.pings_target = 3;
    g.state        = PST_CONNECTING;
    int timeout_s  = 5;

    static struct option longopts[] = {
        {"tg",      required_argument, NULL, 1000},
        {"talk",    no_argument,       NULL, 1001},
        {"hold-ms", required_argument, NULL, 1002},
        {0,0,0,0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:k:u:c:T:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'n': g.node_id = optarg; break;
        case 'k': psk_hex = optarg; break;
        case 'u': snprintf(url, sizeof(url), "%s", optarg); break;
        case 'c': g.pings_target = atoi(optarg); break;
        case 'T': timeout_s = atoi(optarg); break;
        case 1000: g.switch_tg = atoi(optarg); break;
        case 1001: g.talk_then_exit = 1; break;
        case 1002: g.hold_ms = atoi(optarg); g.talk_then_exit = 1; break;
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
    if (!mg_ws_connect(&mgr, url, evh, NULL, NULL)) {
        fprintf(stderr, "probe: connect to %s failed\n", url);
        mg_mgr_free(&mgr);
        return 1;
    }

    time_t  deadline       = time(NULL) + timeout_s;
    int64_t next_refresh_ms = 0;
    while (g.state != PST_DONE && g.state != PST_FAILED &&
           time(NULL) < deadline) {
        mg_mgr_poll(&mgr, 50);
        /* Refresh the floor lease every 500ms while holding so the
         * reflector's hangtime (1500ms) doesn't expire under us. Real
         * mod_link refreshes implicitly via RTP cadence. */
        if (g.hold_until_ms && now_ms() < g.hold_until_ms) {
            int64_t now = now_ms();
            if (now >= next_refresh_ms) {
                for (struct mg_connection *c = mgr.conns; c; c = c->next)
                    if (!c->is_listening && !c->is_closing)
                        mg_ws_send(c, "{\"type\":\"simulate_talk_start\"}",
                                   strlen("{\"type\":\"simulate_talk_start\"}"),
                                   WEBSOCKET_OP_TEXT);
                next_refresh_ms = now + 500;
            }
        }
        if (g.hold_until_ms && now_ms() >= g.hold_until_ms) {
            /* Hold complete: release floor and exit. */
            for (struct mg_connection *c = mgr.conns; c; c = c->next) {
                if (!c->is_listening && !c->is_closing) {
                    mg_ws_send(c, "{\"type\":\"simulate_talk_end\"}",
                               strlen("{\"type\":\"simulate_talk_end\"}"),
                               WEBSOCKET_OP_TEXT);
                    c->is_draining = 1;
                }
            }
            g.state = PST_DONE;
            g.hold_until_ms = 0;
        }
    }
    if (g.state != PST_DONE && g.state != PST_FAILED)
        snprintf(g.fail_reason, sizeof(g.fail_reason),
                 "timeout in state %d after %ds", g.state, timeout_s);

    mg_mgr_free(&mgr);

    if (g.state == PST_DONE) {
        printf("OK\n");
        return 0;
    }
    fprintf(stderr, "FAIL — %s\n", g.fail_reason);
    return 1;
}
