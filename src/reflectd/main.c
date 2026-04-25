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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
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
#include "recordings.h"

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

    /* Sliding window of SRTP auth failures — see PLAN-LINK § 4.6 row
     * "SRTP auth failures from a node (server)". */
    int64_t               srtp_window_start_ms;
    int                   srtp_fail_count;

    /* Server-driven mute. While set, audio fan-out from this node is
     * dropped before the floor check; control plane stays alive. */
    bool                  muted;
} node_rt_t;
static node_rt_t g_node_rt[RCFG_MAX_NODES];

/* Forward decl — used by audio_drain_udp before the function is defined. */
static void send_kicked(struct mg_connection *c, const char *code,
                        const char *msg);
static void send_mute(struct mg_connection *c, const char *reason,
                      int retry_in_s);

/* Lex-compare two version strings of the form "kerchunk X.Y.Z" or
 * "X.Y.Z". Returns <0, 0, >0 like strcmp. Empty min ⇒ always pass. */
static int version_at_least(const char *client_ver, const char *min_ver)
{
    if (!min_ver || !min_ver[0]) return 0;
    if (!client_ver) return -1;
    /* Strip a leading "kerchunk " prefix from either side. */
    const char *cv = client_ver;
    if (strncmp(cv, "kerchunk ", 9) == 0) cv += 9;
    const char *mv = min_ver;
    if (strncmp(mv, "kerchunk ", 9) == 0) mv += 9;
    return strcmp(cv, mv);
}

static int g_udp_fd = -1;

/* Per-talkgroup runtime state (parallel to g_cfg.tgs[]). */
typedef struct {
    int            talker_node_idx;  /* -1 if idle */
    int64_t        lease_expires_ms; /* monotonic ms; 0 if idle */
    rec_session_t *rec;              /* in-flight recording, NULL if none */
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
    int64_t      last_traffic_ms;   /* monotonic — for idle timeout */
    int64_t      last_ping_ms;      /* when we last sent a ping */
    int          waiting_pong;      /* 1 if ping in flight */

    /* Sliding window of malformed JSON for kick threshold — see
     * PLAN-LINK § 4.6 row "Malformed control-plane JSON". */
    int64_t      bad_msg_window_start_ms;
    int          bad_msg_count;
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

/* Build the JSON roster array for a TG: [{"id":"X","connected":bool},...].
 * Returns the byte count written into out (excluding NUL). */
static int build_tg_members_json(int tg_idx, char *out, size_t max)
{
    const rcfg_tg_t *t = &g_cfg.tgs[tg_idx];
    int off = 0;
    if (off < (int)max) out[off++] = '[';
    for (int j = 0; j < t->n_members && off < (int)max - 80; j++) {
        int p = t->member_node_idxs[j];
        off += snprintf(out + off, max - off,
            "%s{\"id\":\"%s\",\"connected\":%s}",
            j == 0 ? "" : ",",
            g_cfg.nodes[p].id,
            g_node_rt[p].connected ? "true" : "false");
    }
    if (off < (int)max) out[off++] = ']';
    if (off < (int)max) out[off]   = '\0';
    return off;
}

/* Push a fresh tg_roster to every connected member of this TG. Called
 * whenever membership state changes — login completes, disconnect, or
 * a node moves in or out via set_tg. */
static void broadcast_tg_roster(int tg_idx)
{
    char members[512];
    build_tg_members_json(tg_idx, members, sizeof(members));
    char buf[640];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"tg\":%u,\"members\":%s}",
             LINK_MSG_TG_ROSTER, g_cfg.tgs[tg_idx].number, members);
    const rcfg_tg_t *t = &g_cfg.tgs[tg_idx];
    for (int j = 0; j < t->n_members; j++) {
        int p = t->member_node_idxs[j];
        if (g_node_rt[p].connected && g_node_rt[p].c)
            send_json(g_node_rt[p].c, buf);
    }
}

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
    if (was_idle || holder_changed) {
        broadcast_talker(tg_idx, node_idx, -1);
        /* New talker → new recording. If a previous holder was still
         * recording (holder_changed without going IDLE first), close
         * its session before opening the next one. */
        if (t->rec) { recordings_end(t->rec); t->rec = NULL; }
        t->rec = recordings_start(g_cfg.tgs[tg_idx].number,
                                   g_cfg.tgs[tg_idx].name,
                                   g_cfg.nodes[node_idx].id);
    }
    return 1;
}

static void floor_release(int tg_idx, const char *reason_for_revoked)
{
    tg_rt_t *t = &g_tg_rt[tg_idx];
    if (t->talker_node_idx < 0) return;
    int holder = t->talker_node_idx;
    t->talker_node_idx  = -1;
    t->lease_expires_ms = 0;
    /* Close the recording for this floor session — writes the WAV header
     * fixups and appends the CDR row. */
    if (t->rec) { recordings_end(t->rec); t->rec = NULL; }
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

/* Walk every connection: send a ping if quiet for keepalive_s, close
 * the conn if no reply within another keepalive_s window. */
static struct mg_mgr *g_mgr_ref;   /* set in main() so the tick can iterate */
static void keepalive_tick(void)
{
    if (!g_mgr_ref) return;
    int64_t now = now_ms();
    int64_t kalive_ms = (int64_t)g_cfg.keepalive_s * 1000;
    for (struct mg_connection *c = g_mgr_ref->conns; c; c = c->next) {
        conn_t *cs = (conn_t *)c->fn_data;
        if (!cs || c->is_listening || c->is_closing || c->is_draining) continue;
        if (cs->state != CST_AUTHENTICATED) continue;

        int64_t quiet = now - cs->last_traffic_ms;
        if (cs->waiting_pong) {
            int64_t since_ping = now - cs->last_ping_ms;
            if (since_ping > kalive_ms) {
                /* No pong → peer is gone. Close cleanly. */
                log_msg("node %s idle timeout — closing",
                        g_cfg.nodes[cs->node_idx].id);
                c->is_closing = 1;
            }
        } else if (quiet > kalive_ms) {
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"%s\",\"seq\":%lld}",
                     LINK_MSG_PING, (long long)now);
            send_json(c, buf);
            cs->last_ping_ms  = now;
            cs->waiting_pong  = 1;
        }
    }
}

/* Send reflector_shutdown(deploy, restart_in_s) to every authenticated
 * conn so they back off cleanly instead of stampeding on a restart. */
static void broadcast_shutdown(int restart_in_s)
{
    if (!g_mgr_ref) return;
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"reason\":\"deploy\",\"restart_in_s\":%d}",
             LINK_MSG_REFLECTOR_SHUTDOWN, restart_in_s);
    for (struct mg_connection *c = g_mgr_ref->conns; c; c = c->next) {
        conn_t *cs = (conn_t *)c->fn_data;
        if (!cs || c->is_listening) continue;
        if (cs->state == CST_AUTHENTICATED)
            send_json(c, buf);
        c->is_draining = 1;
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
             "\"reflector_version\":\"reflectd 0.7.0\","
             "\"min_client_version\":\"%s\","
             "\"proto\":\"%s\"}",
             LINK_MSG_HELLO, chal_hex,
             g_cfg.min_client_version[0] ? g_cfg.min_client_version : "",
             KERCHUNK_LINK_PROTO_VERSION);
    send_json(c, buf);
    cs->state = CST_AWAITING_LOGIN;
    cs->last_traffic_ms = now_ms();
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

static void send_kicked(struct mg_connection *c, const char *code,
                        const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"code\":\"%s\",\"msg\":\"%s\"}",
             LINK_MSG_KICKED, code, msg ? msg : "");
    send_json_then_close(c, buf);
}

static void send_mute(struct mg_connection *c, const char *reason,
                      int retry_in_s)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"reason\":\"%s\",\"retry_in_s\":%d}",
             LINK_MSG_MUTE, reason, retry_in_s);
    send_json(c, buf);
}

static void send_unmute(struct mg_connection *c)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"type\":\"%s\"}", LINK_MSG_UNMUTE);
    send_json(c, buf);
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
    if (version_at_least(client_ver, g_cfg.min_client_version) < 0) {
        send_login_denied(c, LINK_ERR_VERSION_MISMATCH,
                          "client below min_client_version");
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

    /* Build the TG-members list for the dashboard's "who else is on this
     * TG" pane. Lists every node configured for this TG by id+connected. */
    char members[512];
    int tg_idx_for_members = rcfg_tg_idx(&g_cfg, tg);
    if (tg_idx_for_members >= 0)
        build_tg_members_json(tg_idx_for_members, members, sizeof(members));
    else
        snprintf(members, sizeof(members), "[]");

    char buf[KERCHUNK_LINK_MAX_MSG];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"node_id\":\"%s\",\"talkgroup\":%u,"
             "\"keepalive_s\":%d,\"hangtime_ms\":%d,\"proto\":\"%s\","
             "\"rtp_endpoint\":\"%s:%d\","
             "\"srtp_master_key\":\"%s\",\"srtp_master_salt\":\"%s\","
             "\"ssrc\":%u,\"reflector_ssrc\":%u,"
             "\"tg_members\":%s}",
             LINK_MSG_LOGIN_OK, node_id, tg,
             g_cfg.keepalive_s, g_cfg.hangtime_ms,
             KERCHUNK_LINK_PROTO_VERSION,
             g_cfg.rtp_advertise_host, g_cfg.rtp_port,
             key_hex, salt_hex, node_ssrc, refl_ssrc, members);
    send_json(c, buf);

    log_msg("node %s logged in (client %s) on TG %u (ssrc=%u)",
            node_id, client_ver ? client_ver : "?", tg, node_ssrc);

    /* Tell every other member of this TG a fresh roster so their
     * dashboards flip "offline" → "connected" without waiting for the
     * next talker event. */
    if (tg_idx_for_members >= 0)
        broadcast_tg_roster(tg_idx_for_members);

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

    /* Update rosters: the OLD TG loses this node, the NEW TG gains it. */
    if (old_tg && old_tg != tg) {
        int old_tg_idx2 = rcfg_tg_idx(&g_cfg, old_tg);
        if (old_tg_idx2 >= 0) broadcast_tg_roster(old_tg_idx2);
    }
    broadcast_tg_roster(tg_idx);
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
    cs->last_traffic_ms = now_ms();
    cs->waiting_pong = 0;
    struct mg_str body = mg_str_n(line, len);
    char *type = mg_json_get_str(body, "$.type");
    if (!type) {
        /* Track per-conn malformed-msg sliding window; >3 in 60s → kick. */
        int64_t nowm = cs->last_traffic_ms;
        if (nowm - cs->bad_msg_window_start_ms > 60000) {
            cs->bad_msg_window_start_ms = nowm;
            cs->bad_msg_count           = 0;
        }
        if (++cs->bad_msg_count > 3) {
            send_kicked(c, LINK_ERR_PROTOCOL_ERROR,
                        "too many malformed messages");
            return;
        }
        char b[128];
        snprintf(b, sizeof(b),
                 "{\"type\":\"%s\",\"code\":\"%s\",\"msg\":\"no type\"}",
                 LINK_MSG_ERROR, LINK_ERR_PROTOCOL_ERROR);
        send_json(c, b);
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
    } else if (strcmp(type, LINK_MSG_PONG) == 0) {
        /* Already refreshed last_traffic_ms above. */
    } else if (strcmp(type, LINK_MSG_QUALITY) == 0) {
        /* Sustained loss → mute (PLAN-LINK § 4.6). Also broadcast a
         * target_bitrate hint to other senders on the same TG so they
         * back off before the channel collapses. */
        double loss_pct = 0.0;
        mg_json_get_num(body, "$.loss_pct", &loss_pct);
        if (loss_pct > g_cfg.mute_threshold_pct &&
            cs->node_idx >= 0 && !g_node_rt[cs->node_idx].muted) {
            log_msg("node %s loss %.1f%% > threshold %d%% — muting",
                    g_cfg.nodes[cs->node_idx].id, loss_pct,
                    g_cfg.mute_threshold_pct);
            g_node_rt[cs->node_idx].muted = true;
            send_mute(c, "loss_too_high", 60);
        }
        /* If any node on the TG reports >5% loss, suggest 24kbps to
         * everyone on that TG. Recovers up to 32kbps when loss drops. */
        if (cs->node_idx >= 0) {
            uint16_t tg = g_node_rt[cs->node_idx].cur_tg;
            int tgi = rcfg_tg_idx(&g_cfg, tg);
            if (tgi >= 0) {
                int bps = loss_pct > 5.0 ? 24000 : 32000;
                char tb[96];
                snprintf(tb, sizeof(tb),
                         "{\"type\":\"%s\",\"bps\":%d}",
                         LINK_MSG_TARGET_BITRATE, bps);
                for (int j = 0; j < g_cfg.tgs[tgi].n_members; j++) {
                    int peer = g_cfg.tgs[tgi].member_node_idxs[j];
                    if (g_node_rt[peer].connected && g_node_rt[peer].c)
                        send_json(g_node_rt[peer].c, tb);
                }
            }
        }
    } else if (strcmp(type, "simulate_talk_start") == 0) {
        handle_simulate_talk_start(c, cs);
    } else if (strcmp(type, "simulate_talk_end") == 0) {
        handle_simulate_talk_end(cs);
    } else {
        log_msg("unhandled message type '%s'", type);
    }

    free(type);
}

/* ── Admin HTTP API ───────────────────────────────────────────────── */

static void send_basic_auth_required(struct mg_connection *c)
{
    mg_http_reply(c, 401,
        "WWW-Authenticate: Basic realm=\"reflectd\"\r\n"
        "Content-Type: application/json\r\n",
        "{\"error\":\"unauthorized\"}");
}

static int admin_auth_ok(struct mg_http_message *hm)
{
    if (!g_cfg.admin_user[0] || !g_cfg.admin_password[0]) return 0;
    char user[64] = "", pass[128] = "";
    mg_http_creds(hm, user, sizeof(user), pass, sizeof(pass));
    return strcmp(user, g_cfg.admin_user)     == 0 &&
           strcmp(pass, g_cfg.admin_password) == 0;
}

static void send_state_json(struct mg_connection *c)
{
    char body[8192];
    int  off = snprintf(body, sizeof(body),
        "{\"reflector\":\"reflectd\",\"keepalive_s\":%d,\"hangtime_ms\":%d,"
        "\"nodes\":[", g_cfg.keepalive_s, g_cfg.hangtime_ms);
    int first = 1;
    for (int i = 0; i < g_cfg.n_nodes; i++) {
        off += snprintf(body + off, sizeof(body) - off,
            "%s{\"id\":\"%s\",\"connected\":%s,\"muted\":%s,\"tg\":%u,"
            "\"banned\":%s,\"srtp_fail_30s\":%d}",
            first ? "" : ",",
            g_cfg.nodes[i].id,
            g_node_rt[i].connected ? "true" : "false",
            g_node_rt[i].muted     ? "true" : "false",
            g_node_rt[i].cur_tg,
            g_cfg.nodes[i].banned  ? "true" : "false",
            g_node_rt[i].srtp_fail_count);
        first = 0;
    }
    off += snprintf(body + off, sizeof(body) - off, "],\"talkgroups\":[");
    first = 1;
    for (int i = 0; i < g_cfg.n_tgs; i++) {
        const char *talker = (g_tg_rt[i].talker_node_idx >= 0)
            ? g_cfg.nodes[g_tg_rt[i].talker_node_idx].id : NULL;
        off += snprintf(body + off, sizeof(body) - off,
            "%s{\"tg\":%u,\"name\":\"%s\",\"members\":%d,"
            "\"talker\":%s%s%s}",
            first ? "" : ",",
            g_cfg.tgs[i].number, g_cfg.tgs[i].name,
            g_cfg.tgs[i].n_members,
            talker ? "\"" : "", talker ? talker : "null", talker ? "\"" : "");
        first = 0;
    }
    off += snprintf(body + off, sizeof(body) - off, "]}");
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
}

/* {"node_id":"X"[,"reason":"..."][,"retry_in_s":N]} */
static void admin_node_action(struct mg_connection *c,
                              struct mg_http_message *hm,
                              const char *action)
{
    char *node_id = mg_json_get_str(hm->body, "$.node_id");
    if (!node_id) {
        mg_http_reply(c, 400, "", "{\"error\":\"missing node_id\"}");
        return;
    }
    int idx = rcfg_node_idx(&g_cfg, node_id);
    if (idx < 0) {
        mg_http_reply(c, 404, "", "{\"error\":\"unknown node\"}");
        free(node_id);
        return;
    }

    if (!strcmp(action, "kick")) {
        char *reason = mg_json_get_str(hm->body, "$.reason");
        if (g_node_rt[idx].c)
            send_kicked(g_node_rt[idx].c, LINK_ERR_ADMIN_ACTION,
                        reason ? reason : "operator kick");
        log_msg("admin: kicked %s", node_id);
        free(reason);
    } else if (!strcmp(action, "mute")) {
        long retry = mg_json_get_long(hm->body, "$.retry_in_s", 60);
        g_node_rt[idx].muted = true;
        if (g_node_rt[idx].c)
            send_mute(g_node_rt[idx].c, "admin_action", (int)retry);
        log_msg("admin: muted %s (retry_in_s=%ld)", node_id, retry);
    } else if (!strcmp(action, "unmute")) {
        g_node_rt[idx].muted = false;
        if (g_node_rt[idx].c)
            send_unmute(g_node_rt[idx].c);
        log_msg("admin: unmuted %s", node_id);
    }

    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"ok\":true,\"node_id\":\"%s\",\"action\":\"%s\"}",
                  node_id, action);
    free(node_id);
}

/* {"tg":N} */
static void admin_release_floor(struct mg_connection *c,
                                struct mg_http_message *hm)
{
    long tg = mg_json_get_long(hm->body, "$.tg", 0);
    int  tg_idx = rcfg_tg_idx(&g_cfg, (uint16_t)tg);
    if (tg_idx < 0) {
        mg_http_reply(c, 404, "", "{\"error\":\"unknown tg\"}");
        return;
    }
    floor_release(tg_idx, LINK_ERR_ADMIN_ACTION);
    log_msg("admin: released floor on TG %ld", tg);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"ok\":true,\"tg\":%ld}", tg);
}

/* GET /api/recordings — list per-day CSVs that exist on disk, OR with
 * ?date=YYYY-MM-DD return that day's CDR rows as a JSON array. */
static int valid_date_str(const char *s)
{
    if (!s || strlen(s) != 10) return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return 0; }
        else                  { if (s[i] < '0' || s[i] > '9') return 0; }
    }
    return 1;
}

static void api_recordings(struct mg_connection *c, struct mg_http_message *hm)
{
    char date[16] = "";
    int  has_date = mg_http_get_var(&hm->query, "date", date, sizeof(date)) > 0;

    if (!has_date) {
        /* List days that have a CSV. */
        DIR *d = opendir(recordings_dir());
        if (!d) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"days\":[]}");
            return;
        }
        char body[8192];
        int  off = snprintf(body, sizeof(body), "{\"days\":[");
        struct dirent *e;
        int first = 1;
        while ((e = readdir(d)) != NULL) {
            size_t n = strlen(e->d_name);
            if (n != 14 || strcmp(e->d_name + 10, ".csv") != 0) continue;
            char ymd[11]; memcpy(ymd, e->d_name, 10); ymd[10] = '\0';
            if (!valid_date_str(ymd)) continue;
            off += snprintf(body + off, sizeof(body) - off,
                            "%s\"%s\"", first ? "" : ",", ymd);
            first = 0;
        }
        closedir(d);
        snprintf(body + off, sizeof(body) - off, "]}");
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
        return;
    }

    if (!valid_date_str(date)) {
        mg_http_reply(c, 400, "", "{\"error\":\"bad date\"}");
        return;
    }

    char path[300];
    snprintf(path, sizeof(path), "%s/%s.csv", recordings_dir(), date);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"date\":\"%s\",\"records\":[]}", date);
        return;
    }
    char *body = malloc(8192);
    size_t cap = 8192;
    if (!body) { fclose(fp); mg_http_reply(c, 500, "", "{\"error\":\"OOM\"}"); return; }
    size_t blen = (size_t)snprintf(body, cap,
        "{\"date\":\"%s\",\"records\":[", date);

    char line[1024];
    int  first = 1, header_skipped = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (!header_skipped) { header_skipped = 1; continue; }
        /* Each row: ts,date,time,tg,tg_name,node_id,duration,pcm_bytes,recording */
        char *p = line, *cols[10] = {0};
        int n = 0;
        while (*p && n < 9) {
            cols[n++] = p;
            char *comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
        if (n >= 9) {
            cols[8][strcspn(cols[8], "\r\n")] = '\0';
            if (blen + 512 >= cap) {
                cap *= 2;
                char *nb = realloc(body, cap);
                if (!nb) break;
                body = nb;
            }
            /* tg_name / node_id may be CSV-quoted (commas/quotes inside)
             * or bare. Strip quotes if present and emit as JSON strings.
             * For tg_name="Local" the CSV row has bare Local; for a name
             * with a comma it'd be "PNW, OR" and we'd need to undo the
             * RFC 4180 quoting. Lab-grade — replace with a real CSV
             * parser if names get exotic. */
            char *tg_name  = cols[4], *node_id = cols[5];
            size_t tnl = strlen(tg_name);
            if (tnl >= 2 && tg_name[0] == '"' && tg_name[tnl-1] == '"') {
                tg_name[tnl-1] = '\0'; tg_name++;
            }
            size_t nil = strlen(node_id);
            if (nil >= 2 && node_id[0] == '"' && node_id[nil-1] == '"') {
                node_id[nil-1] = '\0'; node_id++;
            }
            blen += (size_t)snprintf(body + blen, cap - blen,
                "%s{\"ts\":%s,\"date\":\"%s\",\"time\":\"%s\",\"tg\":%s,"
                "\"tg_name\":\"%s\",\"node_id\":\"%s\",\"duration_s\":%s,"
                "\"pcm_bytes\":%s,\"recording\":\"%s\"}",
                first ? "" : ",", cols[0], cols[1], cols[2], cols[3],
                tg_name, node_id, cols[6], cols[7], cols[8]);
            first = 0;
        }
    }
    fclose(fp);
    blen += (size_t)snprintf(body + blen, cap - blen, "]}");
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body);
    free(body);
}

/* GET /api/recording?path=YYYY-MM-DD/TGn_HHMMSS_node.wav — stream WAV. */
static void api_recording(struct mg_connection *c, struct mg_http_message *hm)
{
    char rel[256];
    if (mg_http_get_var(&hm->query, "path", rel, sizeof(rel)) <= 0) {
        mg_http_reply(c, 400, "", "{\"error\":\"missing path\"}");
        return;
    }
    /* Realpath sandbox under recording_dir. */
    char dir_real[PATH_MAX];
    if (!realpath(recordings_dir(), dir_real)) {
        mg_http_reply(c, 500, "", "{\"error\":\"recordings_dir unresolved\"}");
        return;
    }
    char cand[PATH_MAX];
    snprintf(cand, sizeof(cand), "%s/%s", dir_real, rel);
    char real[PATH_MAX];
    if (!realpath(cand, real)) {
        mg_http_reply(c, 404, "", "{\"error\":\"not found\"}");
        return;
    }
    size_t dl = strlen(dir_real);
    if (strncmp(real, dir_real, dl) != 0 || real[dl] != '/') {
        mg_http_reply(c, 403, "", "{\"error\":\"path outside recordings_dir\"}");
        return;
    }
    /* .wav extension only. */
    size_t rl = strlen(real);
    if (rl < 4 || strcmp(real + rl - 4, ".wav") != 0) {
        mg_http_reply(c, 400, "", "{\"error\":\".wav only\"}");
        return;
    }
    struct mg_http_serve_opts so = { .root_dir = "/" };
    mg_http_serve_file(c, hm, real, &so);
}

/* SIGHUP handler — config reload. We diff old vs new TG memberships
 * and emit tg_membership_changed for any node that moved. */
static volatile sig_atomic_t g_reload = 0;

static void admin_reload(struct mg_connection *c)
{
    g_reload = 1;
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"ok\":true,\"action\":\"reload_queued\"}");
}

static void route_admin(struct mg_connection *c, struct mg_http_message *hm)
{
    if (!admin_auth_ok(hm)) {
        send_basic_auth_required(c);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/state"), NULL) &&
        mg_match(hm->method, mg_str("GET"), NULL)) {
        send_state_json(c);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/recordings"), NULL) &&
        mg_match(hm->method, mg_str("GET"), NULL)) {
        api_recordings(c, hm);
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/recording"), NULL) &&
        mg_match(hm->method, mg_str("GET"), NULL)) {
        api_recording(c, hm);
        return;
    }
    if (!mg_match(hm->method, mg_str("POST"), NULL)) {
        mg_http_reply(c, 405, "", "{\"error\":\"POST required\"}");
        return;
    }
    if (mg_match(hm->uri, mg_str("/api/admin/kick"),    NULL))
        admin_node_action(c, hm, "kick");
    else if (mg_match(hm->uri, mg_str("/api/admin/mute"),    NULL))
        admin_node_action(c, hm, "mute");
    else if (mg_match(hm->uri, mg_str("/api/admin/unmute"),  NULL))
        admin_node_action(c, hm, "unmute");
    else if (mg_match(hm->uri, mg_str("/api/admin/release-floor"), NULL))
        admin_release_floor(c, hm);
    else if (mg_match(hm->uri, mg_str("/api/admin/reload"),  NULL))
        admin_reload(c);
    else
        mg_http_reply(c, 404, "", "{\"error\":\"unknown admin route\"}");
}

/* Apply a config reload. Reads the current file fresh, diffs each
 * connected node's TG membership, and tells anyone whose default_tg
 * changed (or who lost access entirely) via tg_membership_changed. */
static const char *g_cfg_path;
static void do_reload(void)
{
    rcfg_t newcfg;
    if (rcfg_load(g_cfg_path, &newcfg) < 0) {
        log_msg("reload failed — keeping current config");
        return;
    }

    /* For each currently connected node, see if its allowed_tgs / default
     * tg moved. We only auto-switch nodes whose CURRENT TG is no longer
     * allowed; otherwise we leave them where they are. */
    for (int i = 0; i < g_cfg.n_nodes; i++) {
        if (!g_node_rt[i].connected) continue;
        int new_idx = rcfg_node_idx(&newcfg, g_cfg.nodes[i].id);
        if (new_idx < 0) {
            /* Node was removed from config — kick. */
            if (g_node_rt[i].c)
                send_kicked(g_node_rt[i].c, LINK_ERR_CONFIG_RELOAD,
                            "node removed from config");
            continue;
        }
        if (!rcfg_node_allowed_on_tg(&newcfg, new_idx,
                                     g_node_rt[i].cur_tg)) {
            /* Move to new default_tg if there is one. */
            uint16_t new_tg = newcfg.nodes[new_idx].default_tg;
            if (!new_tg && newcfg.nodes[new_idx].n_allowed_tgs > 0)
                new_tg = newcfg.nodes[new_idx].allowed_tgs[0];
            char buf[200];
            snprintf(buf, sizeof(buf),
                "{\"type\":\"%s\",\"old_tg\":%u,\"new_tg\":%s,"
                "\"reason\":\"config_reload\"}",
                LINK_MSG_TG_MEMBERSHIP_CHANGED,
                g_node_rt[i].cur_tg,
                new_tg ? (char[16]){0} : "null");
            if (new_tg)
                snprintf(buf, sizeof(buf),
                    "{\"type\":\"%s\",\"old_tg\":%u,\"new_tg\":%u,"
                    "\"reason\":\"config_reload\"}",
                    LINK_MSG_TG_MEMBERSHIP_CHANGED,
                    g_node_rt[i].cur_tg, new_tg);
            if (g_node_rt[i].c) send_json(g_node_rt[i].c, buf);
            g_node_rt[i].cur_tg = new_tg;
        }
    }

    g_cfg = newcfg;
    log_msg("config reloaded: %d node(s), %d talkgroup(s)",
            g_cfg.n_nodes, g_cfg.n_tgs);
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

    /* HTTP request:
     *   /link       → WS upgrade for mod_link clients
     *   /api/...    → admin JSON API (Bearer token required)
     *   /admin/...  → static dashboard files (no auth — uses Bearer
     *                 from the form input, JS calls /api/* directly)
     *   /           → 302 redirect to /admin/
     *   anything else → 404
     */
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;
        if (mg_match(hm->uri, mg_str("/link"), NULL)) {
            mg_ws_upgrade(c, hm, NULL);
        } else if (mg_match(hm->uri, mg_str("/api/#"), NULL)) {
            route_admin(c, hm);
        } else if (mg_match(hm->uri, mg_str("/"), NULL)) {
            mg_http_reply(c, 302, "Location: /admin/\r\n", "");
        } else if (mg_match(hm->uri, mg_str("/admin/#"), NULL) ||
                   mg_match(hm->uri, mg_str("/admin"),   NULL)) {
            /* Same Basic Auth gate as /api/* — browsers prompt natively
             * on 401 and cache credentials per origin so the JS doesn't
             * need to manage tokens. */
            if (!admin_auth_ok(hm)) {
                send_basic_auth_required(c);
                return;
            }
            /* Strip the /admin prefix so mg_http_serve_dir maps /admin/X
             * → root_dir/X (and /admin/ → root_dir/index.html). */
            struct mg_http_message rewritten = *hm;
            if (rewritten.uri.len >= 6 &&
                memcmp(rewritten.uri.buf, "/admin", 6) == 0) {
                rewritten.uri.buf += 6;
                rewritten.uri.len -= 6;
                if (rewritten.uri.len == 0) {
                    rewritten.uri.buf = "/";
                    rewritten.uri.len = 1;
                }
            }
            struct mg_http_serve_opts so = { .root_dir = g_cfg.dashboard_dir };
            mg_http_serve_dir(c, &rewritten, &so);
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
                /* Capture which TG this node was on before tearing the
                 * runtime state down — we need it to push the updated
                 * roster to the remaining members. */
                int gone_tg_idx = rcfg_tg_idx(&g_cfg,
                                              g_node_rt[cs->node_idx].cur_tg);

                audio_node_destroy(g_node_rt[cs->node_idx].audio);
                g_node_rt[cs->node_idx].audio     = NULL;
                g_node_rt[cs->node_idx].connected = false;
                g_node_rt[cs->node_idx].c         = NULL;
                log_msg("node %s disconnected",
                        g_cfg.nodes[cs->node_idx].id);

                if (gone_tg_idx >= 0) broadcast_tg_roster(gone_tg_idx);
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

        /* Sliding-window auth-fail tally: if no node auth'd this packet,
         * we don't know who sent it (could be noise), so we can't
         * attribute. But we CAN track per-node fails by trial-auth
         * outcome. For now, count an auth-fail against the SSRC's
         * presumed owner if we can identify one by source-addr match. */
        if (sender_idx < 0) {
            for (int i = 0; i < g_cfg.n_nodes; i++) {
                if (!g_node_rt[i].connected || !g_node_rt[i].audio) continue;
                if (!audio_node_have_addr(g_node_rt[i].audio)) continue;
                struct sockaddr_storage addr; socklen_t addr_len;
                audio_node_get_addr(g_node_rt[i].audio, &addr, &addr_len);
                if (addr_len == src_len &&
                    memcmp(&addr, &src, addr_len) == 0) {
                    int64_t nowm = now_ms();
                    if (nowm - g_node_rt[i].srtp_window_start_ms > 30000) {
                        g_node_rt[i].srtp_window_start_ms = nowm;
                        g_node_rt[i].srtp_fail_count      = 0;
                    }
                    if (++g_node_rt[i].srtp_fail_count > g_cfg.auth_fail_kick) {
                        log_msg("node %s SRTP auth fails > %d/30s — kicking",
                                g_cfg.nodes[i].id, g_cfg.auth_fail_kick);
                        if (g_node_rt[i].c)
                            send_kicked(g_node_rt[i].c, LINK_ERR_AUTH_FAILURES,
                                        "SRTP authentication failure threshold");
                    }
                    break;
                }
            }
            continue;
        }
        find_sender_by_ssrc(ssrc);      /* keep helper used while we're here */

        /* Server-side mute: drop the audio without engaging the floor.
         * Sender's WS stays open so they can see the mute state. */
        if (g_node_rt[sender_idx].muted) continue;

        /* Feed the cleartext Opus payload into the in-flight recording
         * for this TG (if any). decrypted is RTP+payload — payload starts
         * at offset 12 (RTP header). */
        {
            int tgi_for_rec = rcfg_tg_idx(&g_cfg, g_node_rt[sender_idx].cur_tg);
            if (tgi_for_rec >= 0 && g_tg_rt[tgi_for_rec].rec &&
                decrypted_len > 12) {
                /* Engage the floor first (below) so a recording exists
                 * to append to — but we need the audio for THIS packet
                 * recorded too. The floor_try_grant call below will
                 * (re-)create the rec session if this is the first
                 * packet of a new talker. We re-check after that. */
            }
        }

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

        /* Now that the floor is granted, the rec session for this TG
         * (if recording is enabled) exists. Append this packet's Opus
         * payload to it before fan-out. */
        if (g_tg_rt[tg_idx].rec && decrypted_len > 12)
            recordings_append(g_tg_rt[tg_idx].rec,
                              decrypted + 12, decrypted_len - 12);

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
    if (sig == SIGHUP)  { g_reload = 1; return; }
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

    g_cfg_path = cfg_path;
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
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
    if (recordings_global_init(g_cfg.recording_dir,
                               g_cfg.recording_enabled,
                               g_cfg.recording_max_age_days) < 0) {
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

    g_mgr_ref = &mgr;
    while (!g_stop) {
        mg_mgr_poll(&mgr, 10);
        audio_drain_udp();
        floor_tick();
        keepalive_tick();
        if (g_reload) { g_reload = 0; do_reload(); }
    }

    /* Close any in-flight recordings before tearing down. */
    for (int i = 0; i < g_cfg.n_tgs; i++)
        if (g_tg_rt[i].rec) {
            recordings_end(g_tg_rt[i].rec);
            g_tg_rt[i].rec = NULL;
        }
    recordings_global_shutdown();

    log_msg("shutting down — sending reflector_shutdown to clients");
    broadcast_shutdown(15);
    /* Let the WS frames drain; clients re-connect after restart_in_s ±20%. */
    int64_t drain_until = now_ms() + 500;
    while (now_ms() < drain_until)
        mg_mgr_poll(&mgr, 50);
    if (g_udp_fd >= 0) close(g_udp_fd);
    for (int i = 0; i < g_cfg.n_nodes; i++)
        audio_node_destroy(g_node_rt[i].audio);
    audio_global_shutdown();
    mg_mgr_free(&mgr);
    free(g_tls_cert_pem);
    free(g_tls_key_pem);
    return 0;
}
