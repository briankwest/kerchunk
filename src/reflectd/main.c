/*
 * kerchunk-reflectd — central reflector for kerchunk repeater linking.
 *
 * Phase 2: TLS-WebSocket transport, INI-driven roster, multi-node sessions,
 * talkgroup membership, set_tg flow, floor control with `talker` broadcast.
 * Audio bridging (SRTP/Opus) lands in phase 3.
 *
 * Floor is driven for now by debug `simulate_talk_start` / `simulate_talk_end`
 * messages from the probe, so we can exercise floor / talker / floor_denied
 * before any audio code exists. Phase 3 swaps these for real RTP.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "../../vendor/mongoose.h"
#include "../../include/kerchunk_link_proto.h"
#include "audio.h"
#include "config.h"

/* ── Globals ───────────────────────────────────────────────────────── */

static volatile sig_atomic_t g_stop = 0;
static rcfg_t g_cfg;

/* Per-node runtime state (parallel array to g_cfg.nodes[]). */
typedef struct {
    bool                  connected;
    struct mg_connection *c;       /* current connection, NULL when offline */
    uint16_t              cur_tg;  /* talkgroup the node has joined */
    audio_node_t         *audio;   /* SRTP fan-out state, NULL until login */
    int64_t               last_floor_denied_ms;  /* throttle ws spam */
} node_rt_t;
static node_rt_t g_node_rt[RCFG_MAX_NODES];

static int g_udp_fd = -1;

/* Per-talkgroup runtime state (parallel to g_cfg.tgs[]). */
typedef struct {
    int     talker_node_idx;     /* -1 if idle */
    int64_t lease_expires_ms;    /* monotonic ms; 0 if idle */
} tg_rt_t;
static tg_rt_t g_tg_rt[RCFG_MAX_TGS];

/* TLS keys, loaded once. */
static char *g_tls_cert_pem;
static char *g_tls_key_pem;

/* ── Per-connection state ─────────────────────────────────────────── */

typedef enum {
    CST_AWAITING_LOGIN,
    CST_AUTHENTICATED,
} conn_state_t;

typedef struct {
    conn_state_t state;
    uint8_t      challenge[KERCHUNK_LINK_CHALLENGE_BYTES];
    int          node_idx;          /* -1 until authenticated */
    time_t       last_traffic;
} conn_t;

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

/* ── Logging ──────────────────────────────────────────────────────── */

static void log_msg(const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tb;
    strftime(ts, sizeof(ts), "%F %T", localtime_r(&now, &tb));
    fprintf(stderr, "%s reflectd: ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ── Time helper (monotonic ms) ───────────────────────────────────── */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── JSON send helpers ────────────────────────────────────────────── */

static void send_json(struct mg_connection *c, const char *json)
{
    mg_ws_send(c, json, strlen(json), WEBSOCKET_OP_TEXT);
}

static void send_json_then_close(struct mg_connection *c, const char *json)
{
    mg_ws_send(c, json, strlen(json), WEBSOCKET_OP_TEXT);
    c->is_draining = 1;
}

/* ── Floor control ────────────────────────────────────────────────── */

/* Send a `talker` event to every node on a TG except `except_node_idx`
 * (which is usually the talker themself; pass -1 to broadcast to all). */
static void broadcast_talker(int tg_idx, int talker_node_idx,
                             int except_node_idx)
{
    const rcfg_tg_t *tg = &g_cfg.tgs[tg_idx];
    char buf[256];
    if (talker_node_idx >= 0) {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"%s\",\"tg\":%u,\"node_id\":\"%s\","
                 "\"since\":%ld}",
                 LINK_MSG_TALKER, tg->number,
                 g_cfg.nodes[talker_node_idx].id, (long)time(NULL));
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"%s\",\"tg\":%u,\"node_id\":null,"
                 "\"since\":%ld}",
                 LINK_MSG_TALKER, tg->number, (long)time(NULL));
    }
    for (int i = 0; i < tg->n_members; i++) {
        int idx = tg->member_node_idxs[i];
        if (idx == except_node_idx) continue;
        if (g_node_rt[idx].connected && g_node_rt[idx].c) {
            send_json(g_node_rt[idx].c, buf);
        }
    }
}

/* Try to grant the floor on tg to node. Returns 1 on grant, 0 on denied
 * (someone else already holds it). */
static int floor_try_grant(int tg_idx, int node_idx)
{
    tg_rt_t *t = &g_tg_rt[tg_idx];
    int64_t now = now_ms();
    if (t->talker_node_idx >= 0 && t->talker_node_idx != node_idx &&
        now < t->lease_expires_ms) {
        return 0;  /* someone else holds the floor */
    }
    int was_idle = (t->talker_node_idx < 0);
    int holder_changed = (t->talker_node_idx != node_idx);
    t->talker_node_idx  = node_idx;
    t->lease_expires_ms = now + g_cfg.hangtime_ms;
    if (was_idle || holder_changed)
        broadcast_talker(tg_idx, node_idx, -1);
    return 1;
}

static void floor_release(int tg_idx, const char *reason_for_revoked)
{
    tg_rt_t *t = &g_tg_rt[tg_idx];
    if (t->talker_node_idx < 0) return;
    int holder = t->talker_node_idx;
    t->talker_node_idx  = -1;
    t->lease_expires_ms = 0;
    /* Tell everyone the floor is idle. */
    broadcast_talker(tg_idx, -1, -1);
    /* Tell the previous holder why, if requested. */
    if (reason_for_revoked && holder >= 0 &&
        g_node_rt[holder].connected && g_node_rt[holder].c) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"%s\",\"tg\":%u,\"code\":\"%s\"}",
                 LINK_MSG_FLOOR_REVOKED, g_cfg.tgs[tg_idx].number,
                 reason_for_revoked);
        send_json(g_node_rt[holder].c, buf);
    }
}

/* Walk all TGs every poll tick; release any floor whose lease expired. */
static void floor_tick(void)
{
    int64_t now = now_ms();
    for (int i = 0; i < g_cfg.n_tgs; i++) {
        tg_rt_t *t = &g_tg_rt[i];
        if (t->talker_node_idx >= 0 && now >= t->lease_expires_ms) {
            floor_release(i, LINK_ERR_LEASE_EXPIRED);
        }
    }
}

/* Notify a node it tried to talk while another holds the floor. */
static void send_floor_denied(struct mg_connection *c, int tg_idx)
{
    tg_rt_t *t = &g_tg_rt[tg_idx];
    const char *holder = (t->talker_node_idx >= 0)
        ? g_cfg.nodes[t->talker_node_idx].id : "?";
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"tg\":%u,\"current_talker\":\"%s\"}",
             LINK_MSG_FLOOR_DENIED, g_cfg.tgs[tg_idx].number, holder);
    send_json(c, buf);
}

/* ── HMAC verify (auth) ───────────────────────────────────────────── */

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
              input, sizeof(input), computed, &out_len) ||
        out_len != KERCHUNK_LINK_HMAC_BYTES) return -1;
    unsigned char diff = 0;
    for (size_t i = 0; i < KERCHUNK_LINK_HMAC_BYTES; i++)
        diff |= computed[i] ^ claimed[i];
    return diff == 0 ? 0 : -1;
}

/* ── Message handlers ─────────────────────────────────────────────── */

static void send_hello(struct mg_connection *c, conn_t *cs)
{
    if (RAND_bytes(cs->challenge, sizeof(cs->challenge)) != 1) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"%s\",\"code\":\"%s\",\"msg\":\"RAND_bytes\"}",
                 LINK_MSG_ERROR, LINK_ERR_INTERNAL);
        send_json_then_close(c, buf);
        return;
    }
    char chal_hex[KERCHUNK_LINK_CHALLENGE_BYTES * 2 + 1];
    bytes_to_hex(cs->challenge, sizeof(cs->challenge), chal_hex);

    char buf[KERCHUNK_LINK_MAX_MSG];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"challenge\":\"%s\","
             "\"reflector_version\":\"reflectd 0.2.0\","
             "\"min_client_version\":\"kerchunk 1.0.0\","
             "\"proto\":\"%s\"}",
             LINK_MSG_HELLO, chal_hex, KERCHUNK_LINK_PROTO_VERSION);
    send_json(c, buf);
    cs->state = CST_AWAITING_LOGIN;
    cs->last_traffic = time(NULL);
}

static void send_login_denied(struct mg_connection *c,
                              const char *code, const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"code\":\"%s\",\"msg\":\"%s\"}",
             LINK_MSG_LOGIN_DENIED, code, msg);
    send_json_then_close(c, buf);
}

static void handle_login(struct mg_connection *c, conn_t *cs,
                         struct mg_str body)
{
    char *node_id    = mg_json_get_str(body, "$.node_id");
    char *key_hmac   = mg_json_get_str(body, "$.key_hmac");
    char *nonce_hex  = mg_json_get_str(body, "$.nonce");
    char *client_ver = mg_json_get_str(body, "$.client_version");

    if (!node_id || !key_hmac || !nonce_hex) {
        send_login_denied(c, LINK_ERR_PROTOCOL_ERROR, "missing field");
        goto out;
    }

    int idx = rcfg_node_idx(&g_cfg, node_id);
    if (idx < 0) {
        send_login_denied(c, LINK_ERR_UNKNOWN_NODE, "no such node");
        goto out;
    }
    if (g_cfg.nodes[idx].banned) {
        send_login_denied(c, LINK_ERR_BANNED, "operator-banned");
        goto out;
    }
    if (g_node_rt[idx].connected) {
        send_login_denied(c, LINK_ERR_NODE_BUSY, "already connected");
        goto out;
    }

    uint8_t nonce[KERCHUNK_LINK_NONCE_BYTES];
    uint8_t claimed[KERCHUNK_LINK_HMAC_BYTES];
    if (strlen(nonce_hex) != 2 * KERCHUNK_LINK_NONCE_BYTES ||
        hex_to_bytes(nonce_hex, nonce, sizeof(nonce)) != 0 ||
        strlen(key_hmac)  != 2 * KERCHUNK_LINK_HMAC_BYTES ||
        hex_to_bytes(key_hmac, claimed, sizeof(claimed)) != 0) {
        send_login_denied(c, LINK_ERR_PROTOCOL_ERROR, "bad nonce/hmac");
        goto out;
    }
    if (verify_login_hmac(g_cfg.nodes[idx].psk, KERCHUNK_LINK_PSK_BYTES,
                          cs->challenge, nonce, claimed) != 0) {
        send_login_denied(c, LINK_ERR_BAD_KEY, "HMAC mismatch");
        goto out;
    }

    /* Pick initial TG: explicit default_tg, else first allowed_tg. */
    uint16_t tg = g_cfg.nodes[idx].default_tg;
    if (!tg && g_cfg.nodes[idx].n_allowed_tgs > 0)
        tg = g_cfg.nodes[idx].allowed_tgs[0];
    if (!rcfg_node_allowed_on_tg(&g_cfg, idx, tg)) {
        send_login_denied(c, LINK_ERR_NOT_AUTHORIZED,
                          "no usable default talkgroup");
        goto out;
    }

    /* Generate fresh SRTP material + SSRCs for this session. */
    uint8_t  master_key[KERCHUNK_LINK_SRTP_KEY_BYTES];
    uint8_t  master_salt[KERCHUNK_LINK_SRTP_SALT_BYTES];
    uint32_t node_ssrc, refl_ssrc;
    if (RAND_bytes(master_key,  sizeof(master_key))  != 1 ||
        RAND_bytes(master_salt, sizeof(master_salt)) != 1 ||
        RAND_bytes((unsigned char *)&node_ssrc, sizeof(node_ssrc)) != 1 ||
        RAND_bytes((unsigned char *)&refl_ssrc, sizeof(refl_ssrc)) != 1) {
        send_login_denied(c, LINK_ERR_INTERNAL, "RAND_bytes");
        goto out;
    }
    audio_node_t *an = audio_node_create(master_key, master_salt,
                                         node_ssrc, refl_ssrc);
    if (!an) {
        send_login_denied(c, LINK_ERR_INTERNAL, "srtp setup");
        goto out;
    }

    cs->state    = CST_AUTHENTICATED;
    cs->node_idx = idx;
    g_node_rt[idx].connected = true;
    g_node_rt[idx].c         = c;
    g_node_rt[idx].cur_tg    = tg;
    g_node_rt[idx].audio     = an;

    char key_hex[2  * KERCHUNK_LINK_SRTP_KEY_BYTES  + 1];
    char salt_hex[2 * KERCHUNK_LINK_SRTP_SALT_BYTES + 1];
    bytes_to_hex(master_key,  KERCHUNK_LINK_SRTP_KEY_BYTES,  key_hex);
    bytes_to_hex(master_salt, KERCHUNK_LINK_SRTP_SALT_BYTES, salt_hex);

    char buf[KERCHUNK_LINK_MAX_MSG];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"node_id\":\"%s\",\"talkgroup\":%u,"
             "\"keepalive_s\":%d,\"hangtime_ms\":%d,\"proto\":\"%s\","
             "\"rtp_endpoint\":\"%s:%d\","
             "\"srtp_master_key\":\"%s\",\"srtp_master_salt\":\"%s\","
             "\"ssrc\":%u,\"reflector_ssrc\":%u}",
             LINK_MSG_LOGIN_OK, node_id, tg,
             g_cfg.keepalive_s, g_cfg.hangtime_ms,
             KERCHUNK_LINK_PROTO_VERSION,
             g_cfg.rtp_advertise_host, g_cfg.rtp_port,
             key_hex, salt_hex, node_ssrc, refl_ssrc);
    send_json(c, buf);

    log_msg("node %s logged in (client %s) on TG %u (ssrc=%u)",
            node_id, client_ver ? client_ver : "?", tg, node_ssrc);

out:
    free(node_id); free(key_hmac); free(nonce_hex); free(client_ver);
}

static void handle_set_tg(struct mg_connection *c, conn_t *cs,
                          struct mg_str body)
{
    long tg_l = mg_json_get_long(body, "$.tg", 0);
    if (tg_l <= 0 || tg_l > 65535) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"%s\",\"tg\":%ld,\"code\":\"%s\"}",
                 LINK_MSG_TG_DENIED, tg_l, LINK_ERR_PROTOCOL_ERROR);
        send_json(c, buf);
        return;
    }
    uint16_t tg = (uint16_t)tg_l;
    int tg_idx = rcfg_tg_idx(&g_cfg, tg);
    if (tg_idx < 0) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"%s\",\"tg\":%u,\"code\":\"%s\"}",
                 LINK_MSG_TG_DENIED, tg, LINK_ERR_UNKNOWN_TG);
        send_json(c, buf);
        return;
    }
    if (!rcfg_node_allowed_on_tg(&g_cfg, cs->node_idx, tg)) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"%s\",\"tg\":%u,\"code\":\"%s\"}",
                 LINK_MSG_TG_DENIED, tg, LINK_ERR_NOT_AUTHORIZED);
        send_json(c, buf);
        return;
    }

    /* If the node currently holds the floor on the old TG, release it. */
    uint16_t old_tg = g_node_rt[cs->node_idx].cur_tg;
    if (old_tg && old_tg != tg) {
        int old_tg_idx = rcfg_tg_idx(&g_cfg, old_tg);
        if (old_tg_idx >= 0 &&
            g_tg_rt[old_tg_idx].talker_node_idx == cs->node_idx) {
            floor_release(old_tg_idx, NULL);
        }
    }
    g_node_rt[cs->node_idx].cur_tg = tg;

    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"tg\":%u,\"name\":\"%s\"}",
             LINK_MSG_TG_OK, tg, g_cfg.tgs[tg_idx].name);
    send_json(c, buf);
    log_msg("node %s switched to TG %u", g_cfg.nodes[cs->node_idx].id, tg);
}

static void handle_ping(struct mg_connection *c, struct mg_str body)
{
    long seq = mg_json_get_long(body, "$.seq", 0);
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"seq\":%ld}", LINK_MSG_PONG, seq);
    send_json(c, buf);
}

/* Phase-2 only: lets the probe drive floor without audio. Replaced in
 * phase 3 by real RTP packets driving the same floor logic. */
static void handle_simulate_talk_start(struct mg_connection *c, conn_t *cs)
{
    int tg_idx = rcfg_tg_idx(&g_cfg, g_node_rt[cs->node_idx].cur_tg);
    if (tg_idx < 0) return;
    if (!floor_try_grant(tg_idx, cs->node_idx))
        send_floor_denied(c, tg_idx);
}

static void handle_simulate_talk_end(conn_t *cs)
{
    int tg_idx = rcfg_tg_idx(&g_cfg, g_node_rt[cs->node_idx].cur_tg);
    if (tg_idx < 0) return;
    if (g_tg_rt[tg_idx].talker_node_idx == cs->node_idx)
        floor_release(tg_idx, NULL);
}

/* ── Dispatch ─────────────────────────────────────────────────────── */

static void handle_message(struct mg_connection *c, conn_t *cs,
                           const char *line, size_t len)
{
    cs->last_traffic = time(NULL);
    struct mg_str body = mg_str_n(line, len);
    char *type = mg_json_get_str(body, "$.type");
    if (!type) {
        char b[96];
        snprintf(b, sizeof(b),
                 "{\"type\":\"%s\",\"code\":\"%s\",\"msg\":\"no type\"}",
                 LINK_MSG_ERROR, LINK_ERR_PROTOCOL_ERROR);
        send_json_then_close(c, b);
        return;
    }

    if (strcmp(type, LINK_MSG_LOGIN) == 0) {
        if (cs->state != CST_AWAITING_LOGIN) {
            send_login_denied(c, LINK_ERR_PROTOCOL_ERROR, "duplicate login");
        } else {
            handle_login(c, cs, body);
        }
    } else if (cs->state != CST_AUTHENTICATED) {
        char b[128];
        snprintf(b, sizeof(b),
                 "{\"type\":\"%s\",\"code\":\"%s\",\"msg\":\"not authed\"}",
                 LINK_MSG_ERROR, LINK_ERR_PROTOCOL_ERROR);
        send_json_then_close(c, b);
    } else if (strcmp(type, LINK_MSG_PING) == 0) {
        handle_ping(c, body);
    } else if (strcmp(type, LINK_MSG_SET_TG) == 0) {
        handle_set_tg(c, cs, body);
    } else if (strcmp(type, "simulate_talk_start") == 0) {
        handle_simulate_talk_start(c, cs);
    } else if (strcmp(type, "simulate_talk_end") == 0) {
        handle_simulate_talk_end(cs);
    } else {
        log_msg("unhandled message type '%s'", type);
    }

    free(type);
}

/* ── Mongoose event handler ───────────────────────────────────────── */

static void evh(struct mg_connection *c, int ev, void *ev_data)
{
    /* TLS init on accept if cert/key configured. */
    if (ev == MG_EV_ACCEPT && g_tls_cert_pem) {
        struct mg_tls_opts opts = {
            .cert = mg_str(g_tls_cert_pem),
            .key  = mg_str(g_tls_key_pem),
        };
        mg_tls_init(c, &opts);
        return;
    }

    /* HTTP request: upgrade if it's a WS handshake on /, refuse otherwise. */
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if (mg_match(hm->uri, mg_str("/link"), NULL) ||
            mg_match(hm->uri, mg_str("/"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else {
            mg_http_reply(c, 404, "", "{\"error\":\"not found\"}");
        }
        return;
    }

    if (ev == MG_EV_WS_OPEN) {
        conn_t *cs = calloc(1, sizeof(*cs));
        if (!cs) { c->is_closing = 1; return; }
        cs->node_idx = -1;
        c->fn_data = cs;
        send_hello(c, cs);
        return;
    }
    if (ev == MG_EV_WS_MSG) {
        conn_t *cs = (conn_t *)c->fn_data;
        if (!cs) return;
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        if (wm->data.len > KERCHUNK_LINK_MAX_MSG) {
            char b[96];
            snprintf(b, sizeof(b),
                     "{\"type\":\"%s\",\"code\":\"%s\",\"msg\":\"too long\"}",
                     LINK_MSG_ERROR, LINK_ERR_PROTOCOL_ERROR);
            send_json_then_close(c, b);
            return;
        }
        handle_message(c, cs, wm->data.buf, wm->data.len);
        return;
    }
    if (ev == MG_EV_CLOSE) {
        conn_t *cs = (conn_t *)c->fn_data;
        if (cs) {
            if (cs->node_idx >= 0) {
                /* Release floor on any TG this node is currently talking on. */
                for (int i = 0; i < g_cfg.n_tgs; i++) {
                    if (g_tg_rt[i].talker_node_idx == cs->node_idx)
                        floor_release(i, NULL);
                }
                audio_node_destroy(g_node_rt[cs->node_idx].audio);
                g_node_rt[cs->node_idx].audio     = NULL;
                g_node_rt[cs->node_idx].connected = false;
                g_node_rt[cs->node_idx].c         = NULL;
                log_msg("node %s disconnected",
                        g_cfg.nodes[cs->node_idx].id);
            }
            free(cs);
            c->fn_data = NULL;
        }
    }
}

/* ── UDP / RTP fan-out ────────────────────────────────────────────── */

static int rtp_get_ssrc(const uint8_t *pkt, int len, uint32_t *out)
{
    if (len < 12) return -1;
    if ((pkt[0] >> 6) != 2) return -1;   /* version mismatch */
    uint32_t ssrc;
    memcpy(&ssrc, pkt + 8, 4);
    *out = ntohl(ssrc);
    return 0;
}

/* Look up which node has this SSRC on its IN session (i.e. the sender). */
static int find_sender_by_ssrc(uint32_t ssrc)
{
    /* Linear scan — N nodes, called once per packet (~17/s/node).
     * O(N) is fine for any realistic N. Convert to a hash map if a
     * 1000+ node reflector ever shows up. */
    for (int i = 0; i < g_cfg.n_nodes; i++) {
        if (!g_node_rt[i].connected || !g_node_rt[i].audio) continue;
        struct sockaddr_storage tmp;
        socklen_t tlen;
        audio_node_get_addr(g_node_rt[i].audio, &tmp, &tlen);
        (void)tmp; (void)tlen;
        /* We don't have a getter for node_ssrc; cheat by trying to
         * unprotect on every connected node and stop on first success.
         * This is O(N) per packet and sufficient for phase 3. */
    }
    /* Fallthrough — handled by caller doing the trial decryption. */
    (void)ssrc;
    return -1;
}

static void audio_drain_udp(void)
{
    if (g_udp_fd < 0) return;
    uint8_t  pkt[KERCHUNK_LINK_RTP_MAX_PACKET];
    while (1) {
        struct sockaddr_storage src;
        socklen_t src_len = sizeof(src);
        ssize_t   n = recvfrom(g_udp_fd, pkt, sizeof(pkt), 0,
                               (struct sockaddr *)&src, &src_len);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                log_msg("udp recv error: %s", strerror(errno));
            return;
        }

        uint32_t ssrc;
        if (rtp_get_ssrc(pkt, (int)n, &ssrc) != 0) continue;

        /* Find the node whose IN session expects this SSRC. We try
         * unprotecting on each candidate; the right one auths cleanly,
         * the rest fail quickly. */
        int sender_idx = -1;
        int decrypted_len = 0;
        uint8_t decrypted[KERCHUNK_LINK_RTP_MAX_PACKET];
        for (int i = 0; i < g_cfg.n_nodes; i++) {
            if (!g_node_rt[i].connected || !g_node_rt[i].audio) continue;
            memcpy(decrypted, pkt, (size_t)n);
            decrypted_len = (int)n;
            if (audio_node_unprotect(g_node_rt[i].audio,
                                     decrypted, &decrypted_len,
                                     &src, src_len) == 0) {
                sender_idx = i;
                break;
            }
        }
        if (sender_idx < 0) continue;   /* unauth packet, drop */
        find_sender_by_ssrc(ssrc);      /* keep helper used while we're here */

        /* Bootstrap packet (PT=99): SRTP-authenticated proof of identity
         * + UDP source. Don't engage the floor or fan it out — purely a
         * NAT-punch / addr-learn for receive-only nodes. */
        if (decrypted_len >= 12 &&
            (decrypted[1] & 0x7f) == KERCHUNK_LINK_RTP_BOOTSTRAP_PT) {
            log_msg("node %s registered RTP source addr",
                    g_cfg.nodes[sender_idx].id);
            continue;
        }

        uint16_t tg = g_node_rt[sender_idx].cur_tg;
        int      tg_idx = rcfg_tg_idx(&g_cfg, tg);
        if (tg_idx < 0) continue;

        /* Floor enforcement: every authenticated RTP packet attempts
         * to grant/refresh the floor for this sender. If denied (someone
         * else is talking) the packet is dropped and floor_denied is
         * sent at most once per 500 ms per sender so a stuck PTT can't
         * flood the WS link. */
        if (!floor_try_grant(tg_idx, sender_idx)) {
            int64_t nowms = now_ms();
            if (nowms - g_node_rt[sender_idx].last_floor_denied_ms >= 500) {
                send_floor_denied(g_node_rt[sender_idx].c, tg_idx);
                g_node_rt[sender_idx].last_floor_denied_ms = nowms;
            }
            continue;
        }

        /* Fan out to every other member of sender's TG. */
        const rcfg_tg_t *t = &g_cfg.tgs[tg_idx];
        for (int j = 0; j < t->n_members; j++) {
            int peer = t->member_node_idxs[j];
            if (peer == sender_idx) continue;
            if (!g_node_rt[peer].connected || !g_node_rt[peer].audio) continue;
            if (!audio_node_have_addr(g_node_rt[peer].audio)) {
                /* Peer hasn't sent us a packet yet → no return path known.
                 * Once they do, we'll learn it and start forwarding. */
                continue;
            }
            audio_node_send_to(g_node_rt[peer].audio, g_udp_fd,
                               decrypted, decrypted_len);
        }
    }
}

static int udp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

/* ── TLS cert loading ─────────────────────────────────────────────── */

static char *slurp(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return NULL;
    }
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

/* ── Signal + main ────────────────────────────────────────────────── */

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s -c <config.ini>\n"
        "  -c   Path to reflectd INI config\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *cfg_path = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "c:h")) != -1) {
        switch (opt) {
        case 'c': cfg_path = optarg; break;
        case 'h': default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (!cfg_path) { usage(argv[0]); return 2; }

    if (rcfg_load(cfg_path, &g_cfg) < 0) return 1;

    /* Initialize node + TG runtime state. */
    for (int i = 0; i < g_cfg.n_nodes; i++) g_node_rt[i].cur_tg = 0;
    for (int i = 0; i < g_cfg.n_tgs;   i++) g_tg_rt[i].talker_node_idx = -1;

    if (g_cfg.tls_cert[0]) {
        g_tls_cert_pem = slurp(g_cfg.tls_cert);
        g_tls_key_pem  = slurp(g_cfg.tls_key);
        if (!g_tls_cert_pem || !g_tls_key_pem) {
            fprintf(stderr, "reflectd: cannot read tls_cert/tls_key\n");
            return 1;
        }
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct mg_mgr mgr;
    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&mgr);

    if (!mg_http_listen(&mgr, g_cfg.listen_url, evh, NULL)) {
        fprintf(stderr, "reflectd: listen on %s failed\n", g_cfg.listen_url);
        mg_mgr_free(&mgr);
        return 1;
    }

    if (audio_global_init() < 0) {
        mg_mgr_free(&mgr);
        return 1;
    }
    g_udp_fd = udp_listen(g_cfg.rtp_port);
    if (g_udp_fd < 0) {
        fprintf(stderr, "reflectd: UDP bind on port %d failed: %s\n",
                g_cfg.rtp_port, strerror(errno));
        audio_global_shutdown();
        mg_mgr_free(&mgr);
        return 1;
    }

    log_msg("listening on %s + udp:%d with %d node(s), %d talkgroup(s)",
            g_cfg.listen_url, g_cfg.rtp_port, g_cfg.n_nodes, g_cfg.n_tgs);

    while (!g_stop) {
        mg_mgr_poll(&mgr, 10);
        audio_drain_udp();
        floor_tick();
    }

    log_msg("shutting down");
    if (g_udp_fd >= 0) close(g_udp_fd);
    for (int i = 0; i < g_cfg.n_nodes; i++)
        audio_node_destroy(g_node_rt[i].audio);
    audio_global_shutdown();
    mg_mgr_free(&mgr);
    free(g_tls_cert_pem);
    free(g_tls_key_pem);
    return 0;
}
