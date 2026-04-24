/*
 * mod_web.c — Embedded HTTP server for web dashboard
 *
 * Serves static files and JSON API endpoints. SSE for live events.
 * API routes map directly to CLI handlers via kerchunk_resp_t.
 * Uses mongoose (cesanta.com) for HTTP/HTTPS and SSE.
 *
 * TLS is available via mongoose's built-in TLS implementation —
 * no external dependency required. Configure tls_cert and tls_key
 * in [web] to enable HTTPS.
 *
 * Config: [web] section in kerchunk.conf
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "kerchunk_user.h"
#include "kerchunk_queue.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

#define LOG_MOD "web"

/* CORS origin: configurable via [web] cors_origin. Defaults to "*"
 * which is intentional for the embedded dashboard — the real protection
 * is the auth token, not CORS. Users who want to restrict it can set
 * cors_origin to a specific origin in kerchunk.conf. */
static char g_cors_origin[256] = "*";

/* Built at configure time from g_cors_origin */
static char g_api_headers[512] = "";
static char g_cors_headers[512] = "";

static void build_cors_headers(void)
{
    snprintf(g_api_headers, sizeof(g_api_headers),
             "Content-Type: application/json\r\n"
             "Access-Control-Allow-Origin: %s\r\n",
             g_cors_origin);
    snprintf(g_cors_headers, sizeof(g_cors_headers),
             "Access-Control-Allow-Origin: %s\r\n"
             "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
             "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
             "Access-Control-Max-Age: 86400\r\n",
             g_cors_origin);
}

#define API_HEADERS  g_api_headers
#define CORS_HEADERS g_cors_headers

static int is_sensitive(const char *key) {
    static const char *sens[] = {"api_key","auth_password",
                                  "totp_secret","tls_key","google_maps_api_key",
                                  NULL};
    for (int i = 0; sens[i]; i++)
        if (strcmp(key, sens[i]) == 0) return 1;
    return 0;
}

static kerchunk_core_t *g_core;

/* Config */
static int  g_enabled      = 0;
static int  g_port         = 8080;
static char g_bind[64]     = "127.0.0.1";
static char g_auth_user[64]   = "admin";
static char g_auth_password[128] = "";
static int  g_public_only    = 0;
static char g_static_dir[256] = "";
static char g_tls_cert[256] = "";
static char g_tls_key[256]  = "";
static int  g_tls_active    = 0;

/* Registration config */
static int g_registration_enabled = 0;

/* Bulletin config — small markdown file shown on the public page. */
static char g_bulletin_path[256]  = "/var/lib/kerchunk/bulletin.md";
static int  g_bulletin_max_size   = 16384;  /* 16 KB default */

/* Coverage-map image (PNG) publishable from the admin coverage planner.
 * Defaults to a writable state-dir path so the kerchunk user can overwrite
 * it without needing write access to the read-only static web dir. The
 * file is also served verbatim at GET /coverage.png by a dedicated route,
 * so the public page's <img src="coverage.png"> keeps working wherever
 * the file lives. */
static char g_coverage_png_path[256]   = "/var/lib/kerchunk/coverage.png";
static int  g_coverage_png_max_size    = 4 * 1024 * 1024;  /* 4 MB */

/* Admin ACL — CIDR list for admin access.  Clients not matching
 * get a plain 404 with no hint that /admin/ exists. */
#define MAX_ACL_ENTRIES 16

typedef struct {
    uint8_t  addr[16];   /* network address (4 bytes IPv4, 16 bytes IPv6) */
    int      prefix_len; /* CIDR prefix length */
    int      is_ip6;
} acl_entry_t;

static acl_entry_t g_admin_acl[MAX_ACL_ENTRIES];
static int          g_admin_acl_count = 0;

/* PTT config */
static int g_ptt_enabled        = 0;   /* [web] ptt_enabled */
static int g_ptt_max_duration_s = 30;  /* [web] ptt_max_duration */
static int g_ptt_priority       = KERCHUNK_PRI_NORMAL;   /* [web] ptt_priority */

/* Mongoose state */
static struct mg_mgr g_mgr;
static unsigned long g_listener_id;
static int g_web_tid = -1;
static atomic_int g_sse_count;
static atomic_int g_ws_audio_count;
static uint16_t   g_ws_seq;

/* ── WebSocket audio — lock-free SPSC ring (audio thread → web thread) ── */

/* Frame: 4-byte header + frame_samples * 2 bytes (PCM16 LE) */
#define WS_MAX_FRAME_SIZE (4 + KERCHUNK_MAX_FRAME_SAMPLES * 2)

/* Lock-free ring: audio thread writes, web thread reads.
 * No syscalls, no mutexes on the audio thread — just memory writes. */
#define WS_RING_SLOTS 512   /* power of 2, ~10s at 50 frames/sec */
#define WS_RING_MASK  (WS_RING_SLOTS - 1)

typedef struct {
    int16_t samples[KERCHUNK_MAX_FRAME_SAMPLES];
    uint8_t dir;   /* 0x00=RX, 0x01=TX */
} ws_audio_slot_t;

static ws_audio_slot_t g_ws_ring[WS_RING_SLOTS];
static atomic_uint     g_ws_ring_w;   /* write index (audio thread only) */
static atomic_uint     g_ws_ring_r;   /* read index (web thread only) */

/* ── Per-connection WebSocket state (PTT) ── */

#define WS_PTT_MAGIC 0x50545421  /* "PTT!" */

typedef struct {
    uint32_t magic;
    int  authenticated;
    int  admin;          /* 1 if connected via /admin/api/audio */
    int  user_id;
    char user_name[32];
    int  ptt_held;
    size_t audio_len;    /* total samples queued (for duration tracking) */
    int    auth_failures;
    time_t last_auth_failure;
} ws_conn_state_t;

/* Only one user can transmit via WebSocket PTT at a time.
 * Thread safety: all reads/writes occur on the mongoose event loop thread
 * (single-threaded), so no mutex is needed. The one exception is
 * web_unload() which runs on the main thread — see comment there.
 * At shutdown no new events arrive so the race window is benign. */
static ws_conn_state_t *g_ptt_holder;  /* NULL = channel free */

/* Store/retrieve state pointer in c->data.
 * data[0] = 'W' tag for WebSocket connections (vs 'E' for SSE).
 * Pointer stored at data[1..8]. Only dereference after tag check. */
static ws_conn_state_t *ws_get_state(struct mg_connection *c)
{
    if (c->data[0] != 'W') return NULL;
    ws_conn_state_t *st = NULL;
    memcpy(&st, c->data + 1, sizeof(st));
    return st;
}

static void ws_set_state(struct mg_connection *c, ws_conn_state_t *st)
{
    c->data[0] = 'W';
    memcpy(c->data + 1, &st, sizeof(st));
}

/* Push one frame into the ring (called from audio thread) */
static void ws_ring_push(const int16_t *pcm, size_t n, uint8_t dir)
{
    unsigned w = atomic_load_explicit(&g_ws_ring_w, memory_order_relaxed);
    unsigned next = (w + 1) & WS_RING_MASK;
    if (next == atomic_load_explicit(&g_ws_ring_r, memory_order_acquire))
        return;  /* ring full — drop frame */

    int fs = g_core->frame_samples;
    ws_audio_slot_t *s = &g_ws_ring[w];
    size_t copy = n > (size_t)fs ? (size_t)fs : n;
    memcpy(s->samples, pcm, copy * sizeof(int16_t));
    if (copy < (size_t)fs) memset(s->samples + copy, 0, ((size_t)fs - copy) * sizeof(int16_t));
    s->dir = dir;

    atomic_store_explicit(&g_ws_ring_w, next, memory_order_release);
}

/* Flush ring → WebSocket clients (called from web thread before each poll) */
static void ws_flush_ring(void)
{
    if (atomic_load(&g_ws_audio_count) <= 0) return;

    unsigned r = atomic_load_explicit(&g_ws_ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&g_ws_ring_w, memory_order_acquire);

    while (r != w) {
        ws_audio_slot_t *s = &g_ws_ring[r];

        int fs = g_core->frame_samples;
        size_t frame_bytes = (size_t)fs * sizeof(int16_t);
        size_t msg_size = 4 + frame_bytes;
        uint8_t msg[WS_MAX_FRAME_SIZE];
        msg[0] = 0x01;
        msg[1] = s->dir;
        msg[2] = (uint8_t)(g_ws_seq & 0xFF);
        msg[3] = (uint8_t)((g_ws_seq >> 8) & 0xFF);
        g_ws_seq++;
        memcpy(msg + 4, s->samples, frame_bytes);

        for (struct mg_connection *c = g_mgr.conns; c != NULL; c = c->next) {
            if (ws_get_state(c))
                mg_ws_send(c, msg, msg_size, WEBSOCKET_OP_BINARY);
        }

        r = (r + 1) & WS_RING_MASK;
    }

    atomic_store_explicit(&g_ws_ring_r, r, memory_order_release);
}

/* ── Tap handlers (run on audio thread — zero syscalls) ──
 *
 * Only push frames when there's actual activity:
 *   RX tap: only when COR is active (someone is transmitting to us)
 *   TX tap: only when PTT is active (we are transmitting)
 * This prevents streaming mic noise and playback silence to the browser
 * when the repeater is idle. */

static void ws_tx_playback_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (atomic_load(&g_ws_audio_count) <= 0 || !evt->audio.samples)
        return;
    if (!g_core->is_transmitting())
        return;
    /* During an active key-up the software relay also holds PTT, so this
     * tap fires on the same audio the RX tap is already streaming (labeled
     * dir=RX). Two directions for one signal flickers the listener's badge
     * — let the RX tap own the stream until COR drops. Queue-only TX
     * (announcements, CW ID, courtesy tones) still passes through. */
    if (g_core->is_receiving())
        return;

    int fs = g_core->frame_samples;
    const int16_t *src = evt->audio.samples;
    size_t remaining = evt->audio.n;
    while (remaining > 0) {
        size_t n = remaining > (size_t)fs ? (size_t)fs : remaining;
        ws_ring_push(src, n, 0x01);
        src += n;
        remaining -= n;
    }
}

static void ws_rx_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (atomic_load(&g_ws_audio_count) <= 0 || !evt->audio.samples)
        return;
    if (!g_core->is_receiving())
        return;

    int fs = g_core->frame_samples;
    size_t n = evt->audio.n > (size_t)fs ? (size_t)fs : evt->audio.n;
    ws_ring_push(evt->audio.samples, n, 0x00);
}

/* ── WebSocket PTT command handlers ── */

static void ws_send_json(struct mg_connection *c, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        mg_ws_send(c, buf, (size_t)n, WEBSOCKET_OP_TEXT);
}

static void ws_handle_auth(struct mg_connection *c, ws_conn_state_t *st,
                           struct mg_ws_message *wm)
{
    /* Rate limit: block after 5 consecutive failures for 60 seconds */
    if (st->auth_failures >= 5) {
        time_t now = time(NULL);
        if (now - st->last_auth_failure < 60) {
            ws_send_json(c, "{\"ok\":false,\"error\":\"too many failures, wait 60s\"}");
            return;
        }
        st->auth_failures = 0;  /* reset after cooldown */
    }

    char *user = mg_json_get_str(wm->data, "$.user");
    char *pin  = mg_json_get_str(wm->data, "$.pin");

    if (!user || !pin) {
        ws_send_json(c, "{\"ok\":false,\"error\":\"missing user or pin\"}");
        free(user); free(pin);
        return;
    }

    /* Iterate user DB: match username (case-insensitive) AND dtmf_login == pin */
    int count = kerchunk_user_count();
    const kerchunk_user_t *found = NULL;
    for (int i = 0; i < count; i++) {
        const kerchunk_user_t *u = kerchunk_user_get(i);
        if (!u) continue;
        if (strcasecmp(u->username, user) == 0 && strcmp(u->dtmf_login, pin) == 0) {
            found = u;
            break;
        }
    }

    if (!found) {
        st->auth_failures++;
        st->last_auth_failure = time(NULL);
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "WS auth failed for user=%s (checked %d users, failures=%d)",
                    user, count, st->auth_failures);
        ws_send_json(c, "{\"ok\":false,\"error\":\"invalid credentials\"}");
        free(user); free(pin);
        return;
    }

    if (found->access < KERCHUNK_ACCESS_BASIC) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "WS auth denied for user=%s (access=%d)", user, found->access);
        ws_send_json(c, "{\"ok\":false,\"error\":\"insufficient access\"}");
        free(user); free(pin);
        return;
    }

    st->authenticated = 1;
    st->user_id = found->id;
    snprintf(st->user_name, sizeof(st->user_name), "%s", found->name);

    /* Do NOT fire KERCHEVT_CALLER_IDENTIFIED here — that event is for
     * radio caller identification (COR cycle).  Web auth is tracked
     * separately for CDR via the announcement event on PTT off. */

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "WS auth: user=%s id=%d", st->user_name, st->user_id);

    ws_send_json(c, "{\"ok\":true,\"user\":\"%s\",\"ptt_enabled\":%s,"
                 "\"sample_rate\":%d,\"frame_samples\":%d}",
                 st->user_name, g_ptt_enabled ? "true" : "false",
                 g_core->sample_rate, g_core->frame_samples);

    free(user); free(pin);
}

static void ws_handle_ptt_on(struct mg_connection *c, ws_conn_state_t *st)
{
    if (!g_ptt_enabled) {
        ws_send_json(c, "{\"ok\":false,\"error\":\"PTT disabled\"}");
        return;
    }
    if (!st->authenticated) {
        ws_send_json(c, "{\"ok\":false,\"error\":\"not authenticated\"}");
        return;
    }
    if (st->ptt_held) {
        ws_send_json(c, "{\"ok\":false,\"error\":\"PTT already held\"}");
        return;
    }
    if (g_ptt_holder && g_ptt_holder != st) {
        ws_send_json(c, "{\"ok\":false,\"error\":\"channel busy\"}");
        return;
    }

    g_ptt_holder = st;
    st->ptt_held = 1;
    st->audio_len = 0;
    /* No request_ptt here — the main loop's queue logic handles PTT
     * automatically when it sees items in the queue.  This ensures
     * proper tx_delay + CTCSS/DCS ramp-up before audio plays. */

    /* Fire virtual COR so ASR/recorder treat this like an RF transmission */
    kerchevt_t vc = { .type = KERCHEVT_VCOR_ASSERT,
        .vcor = { .source = "web_ptt", .user_id = st->user_id } };
    kerchevt_fire(&vc);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "WS PTT on: user=%s", st->user_name);

    ws_send_json(c, "{\"ok\":true,\"state\":\"transmitting\",\"max_duration\":%d}",
                 g_ptt_max_duration_s);
}

static void ws_handle_ptt_off(struct mg_connection *c, ws_conn_state_t *st)
{
    if (!st->ptt_held) {
        ws_send_json(c, "{\"ok\":false,\"error\":\"PTT not held\"}");
        return;
    }

    st->ptt_held = 0;
    if (g_ptt_holder == st) g_ptt_holder = NULL;
    double duration = (double)st->audio_len / (double)g_core->sample_rate;
    /* No release_ptt — queue system manages PTT automatically.
     * It will hold PTT through tx_tail after the last frame drains. */

    /* Fire virtual COR drop — ASR will capture and transcribe */
    kerchevt_t vc = { .type = KERCHEVT_VCOR_DROP,
        .vcor = { .source = "web_ptt", .user_id = st->user_id } };
    kerchevt_fire(&vc);

    /* Fire announcement for CDR */
    if (st->audio_len > 0) {
        char desc[128];
        snprintf(desc, sizeof(desc), "web_ptt user=%s duration=%.1fs",
                 st->user_name, duration);
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "web_ptt", .description = desc } };
        kerchevt_fire(&ae);
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "WS PTT off: user=%s duration=%.1fs (%zu samples)",
                st->user_name, duration, st->audio_len);

    st->audio_len = 0;

    ws_send_json(c, "{\"ok\":true,\"state\":\"idle\",\"duration\":%.1f}",
                 duration);
}

static void ws_handle_audio_frame(ws_conn_state_t *st,
                                  const uint8_t *data, size_t len)
{
    if (!st->ptt_held) return;
    int fs = g_core->frame_samples;
    size_t expected_len = 1 + (size_t)fs * 2;  /* 1 tag + frame_samples * 2 bytes */
    if (len != expected_len || data[0] != 0x02) return;

    size_t max_samples = (size_t)g_ptt_max_duration_s * (size_t)g_core->sample_rate;
    if (st->audio_len >= max_samples) return;

    /* Queue each frame immediately for real-time playback.
     * Use _src variant to tag source atomically — the frame may drain
     * before a separate tag_item call could find it in the queue. */
    kerchunk_queue_add_buffer_src((const int16_t *)(data + 1), fs,
                                  g_ptt_priority, 0, "web_ptt");
    st->audio_len += fs;
}

/* TLS cert/key data (read once, reused per connection) */
static struct mg_str g_cert_data;
static struct mg_str g_key_data;

/* ── Mongoose log redirect ── */

static char g_mg_log_buf[512];
static int  g_mg_log_pos;

static void mg_log_cb(char ch, void *param)
{
    (void)param;
    if (ch == '\n' || g_mg_log_pos >= (int)sizeof(g_mg_log_buf) - 1) {
        g_mg_log_buf[g_mg_log_pos] = '\0';
        if (g_mg_log_pos > 0 && g_core) {
            /* Mongoose format: "<hex_ts> <level_int> <file:line:func>  <msg>"
             * Map: MG 1=ERROR→LOG_ERROR, 2=INFO→LOG_INFO,
             *      3=DEBUG→LOG_DEBUG, 4=VERBOSE→LOG_DEBUG */
            int level = KERCHUNK_LOG_DEBUG;
            const char *msg = g_mg_log_buf;
            char *p = strchr(g_mg_log_buf, ' ');
            if (p) {
                switch (p[1]) {
                case '1': level = KERCHUNK_LOG_ERROR; break;
                case '2': level = KERCHUNK_LOG_INFO;  break;
                default:  level = KERCHUNK_LOG_DEBUG; break;
                }
                /* Skip "<hex> <level> " to get the rest */
                msg = p + 2;
                while (*msg == ' ') msg++;
            }
            g_core->log(level, LOG_MOD, "%s", msg);
        }
        g_mg_log_pos = 0;
    } else {
        g_mg_log_buf[g_mg_log_pos++] = ch;
    }
}

/* ── JSON string escaping ── */

static void json_escape_str(const char *in, char *out, size_t max)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j < max - 2; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else if ((unsigned char)c < 0x20) { /* skip control chars */ }
        else out[j++] = c;
    }
    out[j] = '\0';
}

/* ── Auth check ── */

/* Constant-time comparison to prevent timing side-channel attacks */
static int ct_compare(const char *a, const char *b, size_t len)
{
    volatile unsigned char result = 0;
    for (size_t i = 0; i < len; i++)
        result |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return result == 0;
}

/* ── Admin ACL ─────────────────────────────────────────────── */

/* Parse a single CIDR entry like "192.168.86.0/24" or "fd00::/8" or "10.0.0.1" */
static int parse_acl_entry(const char *str, acl_entry_t *out)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", str);

    /* Strip whitespace */
    char *s = buf;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    while (len > 0 && s[len-1] == ' ') s[--len] = '\0';
    if (len == 0) return -1;

    /* Split on '/' for prefix length */
    int prefix_len = -1;
    char *slash = strchr(s, '/');
    if (slash) {
        *slash = '\0';
        prefix_len = atoi(slash + 1);
    }

    memset(out, 0, sizeof(*out));

    /* Try IPv6 first */
    struct in6_addr addr6;
    if (inet_pton(AF_INET6, s, &addr6) == 1) {
        memcpy(out->addr, &addr6, 16);
        out->is_ip6 = 1;
        out->prefix_len = (prefix_len >= 0) ? prefix_len : 128;
        return 0;
    }

    /* Try IPv4 */
    struct in_addr addr4;
    if (inet_pton(AF_INET, s, &addr4) == 1) {
        memcpy(out->addr, &addr4, 4);
        out->is_ip6 = 0;
        out->prefix_len = (prefix_len >= 0) ? prefix_len : 32;
        return 0;
    }

    return -1;
}

/* Parse comma-separated ACL: "192.168.86.0/24, 10.0.0.0/8, fd00::/8" */
static void parse_admin_acl(const char *acl_str)
{
    g_admin_acl_count = 0;
    if (!acl_str || !acl_str[0]) return;

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", acl_str);

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);
    while (tok && g_admin_acl_count < MAX_ACL_ENTRIES) {
        acl_entry_t entry;
        if (parse_acl_entry(tok, &entry) == 0) {
            g_admin_acl[g_admin_acl_count++] = entry;
        } else {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "invalid admin_acl entry: %s", tok);
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }
}

/* Check if a client IP matches the admin ACL.
 * Returns 1 if allowed, 0 if denied.
 * If no ACL is configured, all clients are allowed (auth still required). */
static int check_admin_acl(struct mg_connection *c)
{
    if (g_admin_acl_count == 0)
        return 1;  /* no ACL = allow all (auth still gate) */

    for (int i = 0; i < g_admin_acl_count; i++) {
        acl_entry_t *acl = &g_admin_acl[i];

        if (c->rem.is_ip6 != acl->is_ip6) {
            /* Check for IPv4-mapped IPv6: ::ffff:x.x.x.x */
            if (c->rem.is_ip6 && !acl->is_ip6) {
                uint8_t *ip6 = c->rem.addr.ip;
                /* IPv4-mapped IPv6 has bytes 10-11 = 0xff 0xff */
                if (ip6[10] == 0xff && ip6[11] == 0xff) {
                    /* Compare the IPv4 part (bytes 12-15) against the ACL */
                    int bits = acl->prefix_len;
                    int match = 1;
                    for (int b = 0; b < 4 && bits > 0; b++) {
                        uint8_t mask = (bits >= 8) ? 0xff
                                     : (uint8_t)(0xff << (8 - bits));
                        if ((ip6[12 + b] & mask) != (acl->addr[b] & mask)) {
                            match = 0;
                            break;
                        }
                        bits -= 8;
                    }
                    if (match) return 1;
                }
            }
            continue;
        }

        int addr_len = acl->is_ip6 ? 16 : 4;
        int bits = acl->prefix_len;
        int match = 1;

        for (int b = 0; b < addr_len && bits > 0; b++) {
            uint8_t mask = (bits >= 8) ? 0xff : (uint8_t)(0xff << (8 - bits));
            if ((c->rem.addr.ip[b] & mask) != (acl->addr[b] & mask)) {
                match = 0;
                break;
            }
            bits -= 8;
        }

        if (match) return 1;
    }

    return 0;
}

/* HTTP Basic Auth check for admin routes.
 * Uses Mongoose's mg_http_creds() to extract user/pass from Authorization header.
 * Returns 1 if authenticated, 0 if not. */
static int check_basic_auth(struct mg_http_message *hm)
{
    if (g_auth_password[0] == '\0') {
        /* No password configured — only allow when binding to localhost */
        if (strcmp(g_bind, "127.0.0.1") == 0 ||
            strcmp(g_bind, "localhost") == 0 ||
            strcmp(g_bind, "::1") == 0)
            return 1;  /* localhost only */
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "auth rejected: no password configured on non-localhost bind (%s)", g_bind);
        return 0;  /* no password + public bind = deny */
    }

    char user[64] = "", pass[128] = "";
    mg_http_creds(hm, user, sizeof(user), pass, sizeof(pass));

    if (user[0] == '\0' && pass[0] == '\0')
        return 0;  /* no credentials provided */

    size_t ulen = strlen(user);
    size_t alen = strlen(g_auth_user);
    size_t plen = strlen(pass);
    size_t elen = strlen(g_auth_password);

    /* Use bitwise AND to prevent short-circuit: ct_compare always runs
     * regardless of length match, preventing timing side-channel leaks.
     * Compare up to the shorter length to avoid reading past buffer bounds. */
    size_t ucmp = ulen < alen ? ulen : alen;
    size_t pcmp = plen < elen ? plen : elen;
    int user_ok = (ulen == alen) & ct_compare(user, g_auth_user, ucmp ? ucmp : 1);
    int pass_ok = (plen == elen) & ct_compare(pass, g_auth_password, pcmp ? pcmp : 1);

    return user_ok && pass_ok;
}

/* Send 401 with WWW-Authenticate header for Basic Auth */
static void send_basic_auth_required(struct mg_connection *c, int is_api)
{
    if (is_api) {
        mg_http_reply(c, 401,
            "WWW-Authenticate: Basic realm=\"kerchunk\"\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n",
            "{\"error\":\"Authentication required\"}");
    } else {
        mg_http_reply(c, 401,
            "WWW-Authenticate: Basic realm=\"kerchunk\"\r\n"
            "Content-Type: text/plain\r\n",
            "Authentication required\n");
    }
}


/* ── API dispatch ── */

/* Public API paths (no auth required for GET) */
static const char *g_public_apis[] = {
    "/api/status", "/api/weather", "/api/nws", NULL
};

/* Special routes where the URL name doesn't map 1:1 to a CLI command */
typedef struct {
    const char *path;
    const char *cmd;
    const char *arg;
} api_special_route_t;

static const api_special_route_t g_special_routes[] = {
    { "/api/tts",     "tts",    "status" },
    { "/api/modules", "module", "list" },
    { NULL, NULL, NULL }
};

/* Core command iteration (provided by kerchunk_socket.c) */
extern void kerchunk_socket_iter_core_commands(
    void (*cb)(const kerchunk_cli_cmd_t *cmd, void *ud),
    void *ud);

/* ── /api/commands — list all available API commands ── */

#define CMD_LIST_MAX 16384

typedef struct {
    char *buf;
    int   off;
    int   max;
    int   first;
} cmd_list_ctx_t;

static void cmd_list_cb(const kerchunk_cli_cmd_t *cmd, void *ud)
{
    cmd_list_ctx_t *ctx = (cmd_list_ctx_t *)ud;
    if (ctx->off >= ctx->max - 512) return;
    if (!ctx->first) ctx->buf[ctx->off++] = ',';
    ctx->first = 0;

    char e_name[64], e_usage[128], e_desc[256];
    json_escape_str(cmd->name, e_name, sizeof(e_name));
    json_escape_str(cmd->usage ? cmd->usage : cmd->name, e_usage, sizeof(e_usage));
    json_escape_str(cmd->description ? cmd->description : "", e_desc, sizeof(e_desc));

    ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off,
        "{\"name\":\"%s\",\"usage\":\"%s\",\"description\":\"%s\"",
        e_name, e_usage, e_desc);

    if (cmd->category) {
        char e_cat[64];
        json_escape_str(cmd->category, e_cat, sizeof(e_cat));
        ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off,
            ",\"category\":\"%s\"", e_cat);
    }
    if (cmd->ui_label) {
        char e_label[64];
        json_escape_str(cmd->ui_label, e_label, sizeof(e_label));
        ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off,
            ",\"ui_label\":\"%s\"", e_label);
    }
    if (cmd->ui_type)
        ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off,
            ",\"ui_type\":%d", cmd->ui_type);
    if (cmd->ui_command) {
        char e_uicmd[128];
        json_escape_str(cmd->ui_command, e_uicmd, sizeof(e_uicmd));
        ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off,
            ",\"ui_command\":\"%s\"", e_uicmd);
    }

    /* Serialize fields array for FORM types */
    if (cmd->ui_fields && cmd->num_ui_fields > 0) {
        ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off, ",\"fields\":[");
        for (int i = 0; i < cmd->num_ui_fields; i++) {
            const kerchunk_ui_field_t *f = &cmd->ui_fields[i];
            if (i > 0) ctx->buf[ctx->off++] = ',';
            char e_fn[64], e_fl[64], e_ft[32];
            json_escape_str(f->name, e_fn, sizeof(e_fn));
            json_escape_str(f->label, e_fl, sizeof(e_fl));
            json_escape_str(f->type, e_ft, sizeof(e_ft));
            ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off,
                "{\"name\":\"%s\",\"label\":\"%s\",\"type\":\"%s\"",
                e_fn, e_fl, e_ft);
            if (f->options) {
                char e_fo[128];
                json_escape_str(f->options, e_fo, sizeof(e_fo));
                ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off,
                    ",\"options\":\"%s\"", e_fo);
            }
            if (f->placeholder) {
                char e_fp[128];
                json_escape_str(f->placeholder, e_fp, sizeof(e_fp));
                ctx->off += snprintf(ctx->buf + ctx->off, ctx->max - ctx->off,
                    ",\"placeholder\":\"%s\"", e_fp);
            }
            ctx->buf[ctx->off++] = '}';
        }
        ctx->buf[ctx->off++] = ']';
    }

    ctx->buf[ctx->off++] = '}';
}

static void handle_api_commands(struct mg_connection *c)
{
    char buf[CMD_LIST_MAX];
    int off = snprintf(buf, sizeof(buf), "{\"commands\":[");
    cmd_list_ctx_t ctx = { buf, off, (int)sizeof(buf), 1 };

    kerchunk_socket_iter_core_commands(cmd_list_cb, &ctx);
    kerchunk_module_iter_cli_commands(cmd_list_cb, &ctx);

    ctx.off += snprintf(buf + ctx.off, sizeof(buf) - ctx.off, "]}");
    (void)ctx.off;
    mg_http_reply(c, 200, API_HEADERS, "%s", buf);
}

/* ── Dynamic API GET dispatch ── */

static void handle_api_get(struct mg_connection *c, struct mg_http_message *hm)
{
    /* Check special routes first (where URL doesn't map 1:1 to command) */
    for (int i = 0; g_special_routes[i].path; i++) {
        if (!mg_match(hm->uri, mg_str(g_special_routes[i].path), NULL)) continue;

        kerchunk_resp_t resp;
        resp_init(&resp);

        const char *argv[3] = { g_special_routes[i].cmd,
                                 g_special_routes[i].arg, NULL };
        int argc = g_special_routes[i].arg ? 2 : 1;

        kerchunk_dispatch_command(argc, argv, &resp);
        resp_finish(&resp);

        mg_http_reply(c, 200, API_HEADERS, "%s", resp.json);
        return;
    }

    /* /api/commands — list all available commands */
    if (mg_match(hm->uri, mg_str("/api/commands"), NULL)) {
        handle_api_commands(c);
        return;
    }

    /* Dynamic dispatch: extract command name from /api/{name} */
    if (hm->uri.len > 5 && memcmp(hm->uri.buf, "/api/", 5) == 0) {
        char cmd_name[64];
        size_t name_len = hm->uri.len - 5;
        if (name_len >= sizeof(cmd_name)) name_len = sizeof(cmd_name) - 1;
        memcpy(cmd_name, hm->uri.buf + 5, name_len);
        cmd_name[name_len] = '\0';

        /* Reject names with slashes (sub-paths like /api/config/reload) */
        if (!strchr(cmd_name, '/')) {
            kerchunk_resp_t resp;
            resp_init(&resp);

            const char *argv[2] = { cmd_name, NULL };
            int rc = kerchunk_dispatch_command(1, argv, &resp);
            resp_finish(&resp);

            if (rc == 0) {
                mg_http_reply(c, 200, API_HEADERS, "%s", resp.json);
                return;
            }
        }
    }

    mg_http_reply(c, 404, API_HEADERS, "{\"error\":\"Unknown API endpoint\"}");
}

static void handle_api_post_cmd(struct mg_connection *c,
                                 struct mg_http_message *hm)
{
    /* Parse {"cmd":"..."} from body */
    const char *p = mg_json_get_str(hm->body, "$.cmd");
    if (!p) {
        /* Fallback: manual parse */
        const char *s = hm->body.buf;
        const char *f = strstr(s, "\"cmd\":");
        if (!f) {
            mg_http_reply(c, 400, API_HEADERS,
                          "{\"error\":\"Missing cmd field\"}");
            return;
        }
        f += 6;
        while (*f == ' ' || *f == '"') f++;
        char cmd[2048];
        int ci = 0;
        while (*f && *f != '"' && ci < 2047) cmd[ci++] = *f++;
        cmd[ci] = '\0';

        /* Send OK first, then dispatch */
        mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");

        char cmd_copy[2048];
        snprintf(cmd_copy, sizeof(cmd_copy), "%s", cmd);
        const char *argv[32];
        int argc = 0;
        char *cp = cmd_copy;
        while (*cp && argc < 32) {
            while (*cp == ' ') cp++;
            if (!*cp) break;
            if (*cp == '"') {
                cp++;
                argv[argc++] = cp;
                while (*cp && *cp != '"') cp++;
            } else {
                argv[argc++] = cp;
                while (*cp && *cp != ' ') cp++;
            }
            if (*cp) *cp++ = '\0';
        }
        if (argc > 0) {
            kerchunk_resp_t resp;
            resp_init(&resp);
            kerchunk_dispatch_command(argc, argv, &resp);
            resp_finish(&resp);
        }
        return;
    }

    /* Send OK first, then dispatch */
    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");

    char cmd_copy[2048];
    snprintf(cmd_copy, sizeof(cmd_copy), "%s", p);
    free((void *)p);
    const char *argv[32];
    int argc = 0;
    char *cp = cmd_copy;
    while (*cp && argc < 32) {
        while (*cp == ' ') cp++;
        if (!*cp) break;
        if (*cp == '"') {
            cp++;
            argv[argc++] = cp;
            while (*cp && *cp != '"') cp++;
        } else {
            argv[argc++] = cp;
            while (*cp && *cp != ' ') cp++;
        }
        if (*cp) *cp++ = '\0';
    }
    if (argc > 0) {
        kerchunk_resp_t resp;
        resp_init(&resp);
        kerchunk_dispatch_command(argc, argv, &resp);
        resp_finish(&resp);
    }
}

/* ── Users API ── */

static void handle_api_users(struct mg_connection *c)
{
    char buf[RESP_MAX] = "{\"users\":[";
    int off = (int)strlen(buf);
    int count = kerchunk_user_count();
    for (int i = 0; i < count && off < RESP_MAX - 200; i++) {
        const kerchunk_user_t *u = kerchunk_user_get(i);
        if (!u) continue;
        if (i > 0) buf[off++] = ',';
        char e_username[128], e_name[128], e_email[256];
        char e_callsign[32], e_dtmf[16], e_ani[32];
        json_escape_str(u->username, e_username, sizeof(e_username));
        json_escape_str(u->name, e_name, sizeof(e_name));
        json_escape_str(u->email, e_email, sizeof(e_email));
        json_escape_str(u->callsign, e_callsign, sizeof(e_callsign));
        json_escape_str(u->dtmf_login, e_dtmf, sizeof(e_dtmf));
        json_escape_str(u->ani, e_ani, sizeof(e_ani));
        off += snprintf(buf + off, RESP_MAX - off,
            "{\"id\":%d,\"username\":\"%s\",\"name\":\"%s\","
            "\"email\":\"%s\",\"callsign\":\"%s\","
            "\"dtmf_login\":\"%s\",\"ani\":\"%s\",\"access\":%d,"
            "\"voicemail\":%d,\"group\":%d,"
            "\"totp_configured\":%s}",
            u->id, e_username, e_name,
            e_email, e_callsign,
            e_dtmf, e_ani, u->access,
            u->voicemail, u->group,
            u->totp_secret[0] ? "true" : "false");
    }
    off += snprintf(buf + off, RESP_MAX - off, "]}");
    (void)off;
    mg_http_reply(c, 200, API_HEADERS, "%s", buf);
}

/* ── Groups API ── */

static void handle_api_groups(struct mg_connection *c)
{
    char buf[RESP_MAX] = "{\"groups\":[";
    int off = (int)strlen(buf);
    int count = kerchunk_group_count();
    for (int i = 0; i < count && off < RESP_MAX - 200; i++) {
        const kerchunk_group_t *g = kerchunk_group_get(i);
        if (!g) continue;
        if (i > 0) buf[off++] = ',';
        off += snprintf(buf + off, RESP_MAX - off,
            "{\"id\":%d,\"name\":\"%s\"}",
            g->id, g->name);
    }
    off += snprintf(buf + off, RESP_MAX - off, "]}");
    (void)off;
    mg_http_reply(c, 200, API_HEADERS, "%s", buf);
}

/* ── URI id helper ── */

static int uri_id(struct mg_http_message *hm, const char *prefix)
{
    if (hm->uri.len <= strlen(prefix)) return -1;
    return atoi(hm->uri.buf + strlen(prefix));
}

/* ── User CRUD ── */

static void handle_api_user_create(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    char *name = mg_json_get_str(hm->body, "$.name");
    if (!name) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Missing name\"}");
        return;
    }

    kerchunk_core_lock_config();

    /* Find next available user ID (scan up to 9999) */
    int new_id = -1;
    for (int id = 1; id <= 9999; id++) {
        if (!kerchunk_user_lookup_by_id(id)) { new_id = id; break; }
    }
    if (new_id < 0) {
        kerchunk_core_unlock_config();
        free(name);
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Max users reached\"}");
        return;
    }

    char section[32], vbuf[256];
    snprintf(section, sizeof(section), "user.%d", new_id);
    kerchunk_config_set(cfg, section, "name", name);
    free(name);

    char *s;
    long lv;

    s = mg_json_get_str(hm->body, "$.username");
    if (s) { kerchunk_config_set(cfg, section, "username", s); free(s); }

    s = mg_json_get_str(hm->body, "$.email");
    if (s) { kerchunk_config_set(cfg, section, "email", s); free(s); }

    s = mg_json_get_str(hm->body, "$.callsign");
    if (s) {
        for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
        kerchunk_config_set(cfg, section, "callsign", s);
        free(s);
    }

    s = mg_json_get_str(hm->body, "$.dtmf_login");
    kerchunk_config_set(cfg, section, "dtmf_login", s ? s : "");
    free(s);

    s = mg_json_get_str(hm->body, "$.ani");
    kerchunk_config_set(cfg, section, "ani", s ? s : "");
    free(s);

    lv = mg_json_get_long(hm->body, "$.access", 1);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "access", vbuf);

    lv = mg_json_get_long(hm->body, "$.voicemail", 0);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "voicemail", vbuf);

    lv = mg_json_get_long(hm->body, "$.group", 0);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "group", vbuf);

    s = mg_json_get_str(hm->body, "$.totp_secret");
    kerchunk_config_set(cfg, section, "totp_secret", s ? s : "");
    free(s);

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true,\"id\":%d}", new_id);
}

static void handle_api_user_update(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    int id = uri_id(hm, "/api/users/");
    if (id < 1 || id > 9999) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Invalid user ID\"}");
        return;
    }

    kerchunk_core_lock_config();

    char section[32], vbuf[256];
    snprintf(section, sizeof(section), "user.%d", id);

    char *s;
    long lv;

    s = mg_json_get_str(hm->body, "$.name");
    if (s) { kerchunk_config_set(cfg, section, "name", s); free(s); }

    s = mg_json_get_str(hm->body, "$.username");
    if (s) { kerchunk_config_set(cfg, section, "username", s); free(s); }

    s = mg_json_get_str(hm->body, "$.email");
    if (s) { kerchunk_config_set(cfg, section, "email", s); free(s); }

    s = mg_json_get_str(hm->body, "$.callsign");
    if (s) {
        for (char *p = s; *p; p++) *p = toupper((unsigned char)*p);
        kerchunk_config_set(cfg, section, "callsign", s);
        free(s);
    }

    s = mg_json_get_str(hm->body, "$.dtmf_login");
    if (s) { kerchunk_config_set(cfg, section, "dtmf_login", s); free(s); }

    s = mg_json_get_str(hm->body, "$.ani");
    if (s) { kerchunk_config_set(cfg, section, "ani", s); free(s); }

    lv = mg_json_get_long(hm->body, "$.access", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "access", vbuf); }

    lv = mg_json_get_long(hm->body, "$.voicemail", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "voicemail", vbuf); }

    lv = mg_json_get_long(hm->body, "$.group", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "group", vbuf); }

    s = mg_json_get_str(hm->body, "$.totp_secret");
    if (s) { kerchunk_config_set(cfg, section, "totp_secret", s); free(s); }

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

static void handle_api_user_delete(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    int id = uri_id(hm, "/api/users/");
    if (id < 1 || id > 9999) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Invalid user ID\"}");
        return;
    }

    kerchunk_core_lock_config();

    char section[32];
    snprintf(section, sizeof(section), "user.%d", id);
    kerchunk_config_remove_section(cfg, section);
    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

/* ── Group CRUD ── */

static void handle_api_group_create(struct mg_connection *c,
                                     struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    char *name = mg_json_get_str(hm->body, "$.name");
    if (!name) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Missing name\"}");
        return;
    }

    kerchunk_core_lock_config();

    /* Find next available group ID */
    int new_id = -1;
    for (int id = 1; id <= KERCHUNK_MAX_GROUPS; id++) {
        if (!kerchunk_group_lookup_by_id(id)) { new_id = id; break; }
    }
    if (new_id < 0) {
        kerchunk_core_unlock_config();
        free(name);
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Max groups reached\"}");
        return;
    }

    char section[32];
    snprintf(section, sizeof(section), "group.%d", new_id);
    kerchunk_config_set(cfg, section, "name", name);
    free(name);

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true,\"id\":%d}", new_id);
}

static void handle_api_group_update(struct mg_connection *c,
                                     struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    int id = uri_id(hm, "/api/groups/");
    if (id < 1 || id > KERCHUNK_MAX_GROUPS) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Invalid group ID\"}");
        return;
    }

    kerchunk_core_lock_config();

    char section[32];
    snprintf(section, sizeof(section), "group.%d", id);

    char *s;
    s = mg_json_get_str(hm->body, "$.name");
    if (s) { kerchunk_config_set(cfg, section, "name", s); free(s); }

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

static void handle_api_group_delete(struct mg_connection *c,
                                     struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    int id = uri_id(hm, "/api/groups/");
    if (id < 1 || id > KERCHUNK_MAX_GROUPS) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Invalid group ID\"}");
        return;
    }

    kerchunk_core_lock_config();

    char section[32];
    snprintf(section, sizeof(section), "group.%d", id);
    kerchunk_config_remove_section(cfg, section);
    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

/* ── Config API ── */

static void handle_api_config_get(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    (void)hm;
    if (g_auth_password[0] == '\0') {
        mg_http_reply(c, 403, API_HEADERS,
                      "{\"error\":\"auth_password not configured\"}");
        return;
    }

    kerchunk_config_t *cfg = kerchunk_core_get_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    kerchunk_core_lock_config();

    char buf[16384] = "{\"sections\":{";
    int off = (int)strlen(buf);
    int sec_iter = 0;
    const char *section;
    int first_sec = 1;

    while ((section = kerchunk_config_next_section(cfg, &sec_iter)) != NULL) {
        if (strncmp(section, "user.", 5) == 0 ||
            strncmp(section, "group.", 6) == 0)
            continue;

        if (!first_sec && off < (int)sizeof(buf) - 2)
            buf[off++] = ',';
        first_sec = 0;

        char e_sec[128];
        json_escape_str(section, e_sec, sizeof(e_sec));
        off += snprintf(buf + off, sizeof(buf) - off, "\"%s\":{", e_sec);

        int key_iter = 0;
        const char *key, *val;
        int first_key = 1;
        while ((key = kerchunk_config_next_key(cfg, section, &key_iter,
                                                &val)) != NULL) {
            if (!first_key && off < (int)sizeof(buf) - 2)
                buf[off++] = ',';
            first_key = 0;

            const char *show = is_sensitive(key) ? "********" : val;
            char e_key[128], e_val[512];
            json_escape_str(key, e_key, sizeof(e_key));
            json_escape_str(show, e_val, sizeof(e_val));
            off += snprintf(buf + off, sizeof(buf) - off,
                            "\"%s\":\"%s\"", e_key, e_val);

            if (off >= (int)sizeof(buf) - 64) break;
        }

        off += snprintf(buf + off, sizeof(buf) - off, "}");
        if (off >= (int)sizeof(buf) - 64) break;
    }

    off += snprintf(buf + off, sizeof(buf) - off, "}}");
    (void)off;
    kerchunk_core_unlock_config();
    mg_http_reply(c, 200, API_HEADERS, "%s", buf);
}

static void handle_api_config_put(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    if (g_auth_password[0] == '\0') {
        mg_http_reply(c, 403, API_HEADERS,
                      "{\"error\":\"auth_password not configured\"}");
        return;
    }

    kerchunk_config_t *cfg = kerchunk_core_get_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    char *section = mg_json_get_str(hm->body, "$.section");
    if (!section) {
        mg_http_reply(c, 400, API_HEADERS,
                      "{\"error\":\"Missing section\"}");
        return;
    }

    if (strncmp(section, "user.", 5) == 0 ||
        strncmp(section, "group.", 6) == 0) {
        free(section);
        mg_http_reply(c, 403, API_HEADERS,
                      "{\"error\":\"Use users/groups API\"}");
        return;
    }

    /* Find "values" object in body and iterate key:value pairs */
    const char *vstart = strstr(hm->body.buf, "\"values\"");
    if (!vstart) {
        free(section);
        mg_http_reply(c, 400, API_HEADERS,
                      "{\"error\":\"Missing values\"}");
        return;
    }

    kerchunk_core_lock_config();
    vstart = strchr(vstart, '{');
    if (vstart) {
        vstart++; /* skip { */
        char key[64], val[256];
        while (*vstart && *vstart != '}') {
            while (*vstart && (*vstart == ' ' || *vstart == ',' ||
                               *vstart == '\n' || *vstart == '\r' ||
                               *vstart == '\t'))
                vstart++;
            if (*vstart == '"') {
                vstart++;
                int ki = 0;
                while (*vstart && *vstart != '"' && ki < 63)
                    key[ki++] = *vstart++;
                key[ki] = '\0';
                if (*vstart == '"') vstart++;
                while (*vstart && *vstart != ':') vstart++;
                if (*vstart == ':') vstart++;
                while (*vstart == ' ') vstart++;
                if (*vstart == '"') {
                    vstart++;
                    int vi = 0;
                    while (*vstart && *vstart != '"' && vi < 255)
                        val[vi++] = *vstart++;
                    val[vi] = '\0';
                    if (*vstart == '"') vstart++;

                    if (is_sensitive(key) && strcmp(val, "********") == 0)
                        continue;
                    kerchunk_config_set(cfg, section, key, val);
                }
            } else {
                break;
            }
        }
    }

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);
    free(section);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

/* ── Registration handler ── */

/* Cryptographically secure random integer from /dev/urandom */
static int secure_random_int(int min, int max)
{
    unsigned int r;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, &r, sizeof(r)) != (ssize_t)sizeof(r)) {
        if (fd >= 0) close(fd);
        return min;
    }
    close(fd);
    return min + (int)(r % (unsigned)(max - min + 1));
}

/* Simple rate limit: track last registration time, enforce minimum gap */
static time_t g_last_register_time;
#define REGISTER_RATE_LIMIT_S 10  /* one registration per 10 seconds */

/* Per-IP registration rate limit */
#define REG_RATE_LIMIT_SIZE 16
#define REG_RATE_LIMIT_WINDOW 3600  /* 1 hour */
#define REG_RATE_LIMIT_MAX 5       /* max registrations per IP per hour */

static struct {
    uint8_t ip[16];
    int     is_ip6;
    int     count;
    time_t  first_attempt;
} g_reg_rate[REG_RATE_LIMIT_SIZE];
static int g_reg_rate_idx = 0;

/* ── Bulletin ── */

/* Append src to dst as a JSON string body (no surrounding quotes) — escapes
 * the minimum required by RFC 8259. Caller must reserve 1 byte for NUL. */
static void json_escape_append(char *dst, size_t max, const char *src, size_t n)
{
    size_t len = strlen(dst);
    for (size_t i = 0; i < n && len + 8 < max; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch == '"' || ch == '\\') {
            dst[len++] = '\\'; dst[len++] = (char)ch;
        } else if (ch == '\n') {
            dst[len++] = '\\'; dst[len++] = 'n';
        } else if (ch == '\r') {
            dst[len++] = '\\'; dst[len++] = 'r';
        } else if (ch == '\t') {
            dst[len++] = '\\'; dst[len++] = 't';
        } else if (ch < 0x20) {
            len += snprintf(dst + len, max - len, "\\u%04x", ch);
        } else {
            dst[len++] = (char)ch;
        }
    }
    dst[len] = '\0';
}

/* Render the current bulletin as a self-contained JSON payload:
 *   {"text":"<escaped>","updated_at":<mtime>,"empty":<bool>}
 *
 * Returns a malloc'd string the caller must free. NULL on OOM. Used by
 * both the GET handler and the SSE publish path so the wire shape is
 * identical. */
static char *build_bulletin_json(void)
{
    FILE *fp = fopen(g_bulletin_path, "r");
    if (!fp) {
        char *empty = strdup("{\"text\":\"\",\"updated_at\":0,\"empty\":true}");
        return empty;
    }

    size_t cap = (size_t)g_bulletin_max_size;
    char *buf = malloc(cap + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, cap, fp);
    fclose(fp);
    buf[n] = '\0';

    struct stat st;
    long mtime = 0;
    if (stat(g_bulletin_path, &st) == 0) mtime = (long)st.st_mtime;

    /* Worst-case: every character escapes to six bytes (\uXXXX). */
    size_t jsz = n * 6 + 128;
    char *json = malloc(jsz);
    if (!json) { free(buf); return NULL; }
    snprintf(json, jsz, "{\"text\":\"");
    json_escape_append(json, jsz, buf, n);
    size_t l = strlen(json);
    snprintf(json + l, jsz - l, "\",\"updated_at\":%ld,\"empty\":%s}",
             mtime, n == 0 ? "true" : "false");
    free(buf);
    return json;
}

/* Forward declaration — publish_status_snapshot() is defined near
 * sse_publish_impl below but is called from the WebSocket
 * connect/disconnect paths above it. */
static void publish_status_snapshot(void);

/* Build the current bulletin payload and push it as a bulletin_updated
 * SSE event. Safe to call before g_core->sse_publish is wired (no-op). */
static void publish_bulletin_snapshot(void)
{
    if (!g_core || !g_core->sse_publish) return;
    char *json = build_bulletin_json();
    if (!json) return;
    g_core->sse_publish("bulletin_updated", json, /*admin_only=*/0);
    free(json);
}

static void handle_api_bulletin_get(struct mg_connection *c)
{
    char *json = build_bulletin_json();
    if (!json) { mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"oom\"}"); return; }
    mg_http_reply(c, 200, API_HEADERS, "%s", json);
    free(json);
}

static void handle_api_bulletin_put(struct mg_connection *c,
                                     struct mg_http_message *hm)
{
    size_t n = hm->body.len;
    if (n > (size_t)g_bulletin_max_size) {
        mg_http_reply(c, 413, API_HEADERS,
                      "{\"error\":\"bulletin too large\"}");
        return;
    }

    /* Atomic write via tmpfile + rename so a concurrent GET never sees a
     * half-written file. */
    char tmp_path[300];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", g_bulletin_path);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "bulletin: cannot open %s: %s", tmp_path, strerror(errno));
        mg_http_reply(c, 500, API_HEADERS,
                      "{\"error\":\"cannot open bulletin file\"}");
        return;
    }
    if (n > 0 && fwrite(hm->body.buf, 1, n, fp) != n) {
        fclose(fp);
        unlink(tmp_path);
        mg_http_reply(c, 500, API_HEADERS,
                      "{\"error\":\"write failed\"}");
        return;
    }
    fclose(fp);
    if (rename(tmp_path, g_bulletin_path) != 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "bulletin: rename failed: %s", strerror(errno));
        unlink(tmp_path);
        mg_http_reply(c, 500, API_HEADERS,
                      "{\"error\":\"rename failed\"}");
        return;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "bulletin updated (%zu bytes)", n);
    /* Push the new content to every live SSE client and refresh the
     * snapshot cache so future connectors see the latest edit. */
    publish_bulletin_snapshot();
    mg_http_reply(c, 200, API_HEADERS,
                  "{\"ok\":true,\"bytes\":%zu}", n);
}

/* ── Coverage map PNG publish/serve ──
 *
 * GET  /coverage.png          — public, serves the configured file
 * PUT  /admin/api/coverage-png — admin, writes body (raw PNG bytes)
 *
 * The coverage planner (web/admin/coverage.html) composes a PNG blob
 * client-side and POSTs it here. The public page's existing
 * <img src="coverage.png"> on /api/index then picks up the update on
 * the next reload. */

static void handle_api_coverage_png_get(struct mg_connection *c,
                                         struct mg_http_message *hm)
{
    struct mg_http_serve_opts opts = {
        .mime_types    = "png=image/png",
        /* Let the browser revalidate with If-Modified-Since on each
         * load so a newly-published image is visible immediately.
         * 304s keep it cheap when nothing changed. */
        .extra_headers = "Cache-Control: no-cache\r\n",
    };
    mg_http_serve_file(c, hm, g_coverage_png_path, &opts);
}

static void handle_api_coverage_png_put(struct mg_connection *c,
                                         struct mg_http_message *hm)
{
    size_t n = hm->body.len;
    if (n == 0) {
        mg_http_reply(c, 400, API_HEADERS,
                      "{\"error\":\"empty body\"}");
        return;
    }
    if (n > (size_t)g_coverage_png_max_size) {
        mg_http_reply(c, 413, API_HEADERS,
                      "{\"error\":\"coverage PNG too large\"}");
        return;
    }
    /* Full 8-byte PNG signature check. Rejecting non-PNG bodies keeps a
     * malformed upload (or a typo'd curl) from landing on a path the
     * public site happily serves. We don't parse the rest of the file —
     * the browser will catch any structural corruption and just fail
     * to render, which is an acceptable failure mode. */
    static const unsigned char png_sig[8] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A
    };
    if (n < sizeof(png_sig) ||
        memcmp(hm->body.buf, png_sig, sizeof(png_sig)) != 0) {
        mg_http_reply(c, 400, API_HEADERS,
                      "{\"error\":\"body is not a PNG\"}");
        return;
    }

    /* PID-suffixed tmp path so concurrent PUTs don't fight over one
     * shared tempfile and produce an interleaved-write corrupt final.
     * Each PUT runs on the same mongoose thread today, but the suffix
     * is cheap insurance against any future threading changes and also
     * keeps a crash-during-write from leaving a permanent .tmp lying
     * around under the same predictable name. */
    char tmp_path[320];
    snprintf(tmp_path, sizeof(tmp_path), "%s.%d.tmp",
             g_coverage_png_path, (int)getpid());
    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "coverage.png: cannot open %s: %s",
                    tmp_path, strerror(errno));
        mg_http_reply(c, 500, API_HEADERS,
                      "{\"error\":\"cannot open coverage file\"}");
        return;
    }
    if (fwrite(hm->body.buf, 1, n, fp) != n) {
        fclose(fp);
        unlink(tmp_path);
        mg_http_reply(c, 500, API_HEADERS,
                      "{\"error\":\"write failed\"}");
        return;
    }
    fclose(fp);
    if (rename(tmp_path, g_coverage_png_path) != 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "coverage.png: rename failed: %s", strerror(errno));
        unlink(tmp_path);
        mg_http_reply(c, 500, API_HEADERS,
                      "{\"error\":\"rename failed\"}");
        return;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "coverage.png published (%zu bytes)", n);
    mg_http_reply(c, 200, API_HEADERS,
                  "{\"ok\":true,\"bytes\":%zu}", n);
}

static void handle_api_register(struct mg_connection *c,
                                 struct mg_http_message *hm)
{
    if (!g_registration_enabled) {
        mg_http_reply(c, 403, API_HEADERS,
                      "{\"error\":\"Registration disabled\"}");
        return;
    }

    time_t now = time(NULL);
    if (now - g_last_register_time < REGISTER_RATE_LIMIT_S) {
        mg_http_reply(c, 429, API_HEADERS,
                      "{\"error\":\"Too many requests, try again later\"}");
        return;
    }

    /* Per-IP rate limit */
    {
        int found = -1;
        for (int i = 0; i < REG_RATE_LIMIT_SIZE; i++) {
            if (g_reg_rate[i].is_ip6 == c->rem.is_ip6 &&
                memcmp(g_reg_rate[i].ip, c->rem.addr.ip, c->rem.is_ip6 ? 16 : 4) == 0) {
                found = i;
                break;
            }
        }
        if (found >= 0) {
            if (now - g_reg_rate[found].first_attempt < REG_RATE_LIMIT_WINDOW) {
                if (g_reg_rate[found].count >= REG_RATE_LIMIT_MAX) {
                    mg_http_reply(c, 429, API_HEADERS,
                                  "{\"error\":\"too many registrations, try again later\"}");
                    return;
                }
                g_reg_rate[found].count++;
            } else {
                g_reg_rate[found].first_attempt = now;
                g_reg_rate[found].count = 1;
            }
        } else {
            int idx = g_reg_rate_idx++ % REG_RATE_LIMIT_SIZE;
            memcpy(g_reg_rate[idx].ip, c->rem.addr.ip, 16);
            g_reg_rate[idx].is_ip6 = c->rem.is_ip6;
            g_reg_rate[idx].first_attempt = now;
            g_reg_rate[idx].count = 1;
        }
    }

    char *username = mg_json_get_str(hm->body, "$.username");
    if (!username || username[0] == '\0') {
        free(username);
        mg_http_reply(c, 400, API_HEADERS,
                      "{\"error\":\"Missing username\"}");
        return;
    }

    /* Validate: lowercase, no spaces, truncate to 31 */
    for (int i = 0; username[i]; i++) {
        if (username[i] == ' ' || (unsigned char)username[i] != (unsigned char)tolower((unsigned char)username[i])) {
            free(username);
            mg_http_reply(c, 400, API_HEADERS,
                "{\"error\":\"Username must be lowercase with no spaces\"}");
            return;
        }
    }
    if (strlen(username) > 31) username[31] = '\0';

    /* Check duplicate */
    if (kerchunk_user_lookup_by_username(username)) {
        free(username);
        mg_http_reply(c, 409, API_HEADERS,
                      "{\"error\":\"Username already taken\"}");
        return;
    }

    char *name_str = mg_json_get_str(hm->body, "$.name");
    char *email_str = mg_json_get_str(hm->body, "$.email");
    char *callsign_str = mg_json_get_str(hm->body, "$.callsign");
    /* Force callsign uppercase */
    if (callsign_str) {
        for (char *p = callsign_str; *p; p++) *p = toupper((unsigned char)*p);
    }

    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        free(username); free(name_str); free(email_str); free(callsign_str);
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    kerchunk_core_lock_config();

    /* Find next available user ID */
    int new_id = -1;
    for (int id = 1; id <= 9999; id++) {
        if (!kerchunk_user_lookup_by_id(id)) { new_id = id; break; }
    }
    if (new_id < 0) {
        kerchunk_core_unlock_config();
        free(username); free(name_str); free(email_str); free(callsign_str);
        mg_http_reply(c, 503, API_HEADERS,
                      "{\"error\":\"No user slots available\"}");
        return;
    }

    /* Generate random 4-digit DTMF login (1000-9999), collision check */
    char dtmf_login[8] = "";
    int attempts = 0;
    while (attempts < 100) {
        int code = secure_random_int(1000, 9999);
        snprintf(dtmf_login, sizeof(dtmf_login), "%d", code);
        /* Check collision */
        int collision = 0;
        int ucount = kerchunk_user_count();
        for (int i = 0; i < ucount; i++) {
            const kerchunk_user_t *u = kerchunk_user_get(i);
            if (u && strcmp(u->dtmf_login, dtmf_login) == 0) {
                collision = 1; break;
            }
        }
        if (!collision) break;
        attempts++;
    }
    if (attempts >= 100) {
        kerchunk_core_unlock_config();
        free(username); free(name_str); free(email_str); free(callsign_str);
        mg_http_reply(c, 503, API_HEADERS,
                      "{\"error\":\"Could not generate unique DTMF login\"}");
        return;
    }

    /* Generate random 5-digit ANI (10000-99999), collision check */
    char ani[16] = "";
    attempts = 0;
    while (attempts < 100) {
        int code = secure_random_int(10000, 99999);
        snprintf(ani, sizeof(ani), "%d", code);
        if (!kerchunk_user_lookup_by_ani(ani)) break;
        attempts++;
    }
    if (attempts >= 100) {
        kerchunk_core_unlock_config();
        free(username); free(name_str); free(email_str); free(callsign_str);
        mg_http_reply(c, 503, API_HEADERS,
                      "{\"error\":\"Could not generate unique ANI\"}");
        return;
    }

    /* Write config section */
    char section[32];
    snprintf(section, sizeof(section), "user.%d", new_id);
    kerchunk_config_set(cfg, section, "username", username);
    kerchunk_config_set(cfg, section, "name",
                        (name_str && name_str[0]) ? name_str : username);
    if (email_str && email_str[0])
        kerchunk_config_set(cfg, section, "email", email_str);
    if (callsign_str && callsign_str[0])
        kerchunk_config_set(cfg, section, "callsign", callsign_str);
    kerchunk_config_set(cfg, section, "dtmf_login", dtmf_login);
    kerchunk_config_set(cfg, section, "ani", ani);
    kerchunk_config_set(cfg, section, "access", "1");
    kerchunk_config_set(cfg, section, "voicemail", "0");
    kerchunk_config_set(cfg, section, "group", "0");

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    const char *display_name = (name_str && name_str[0]) ? name_str : username;

    g_last_register_time = time(NULL);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "registered user %d: %s [%s]", new_id, display_name, username);

    mg_http_reply(c, 200, API_HEADERS,
        "{\"ok\":true,\"id\":%d,\"username\":\"%s\",\"name\":\"%s\","
        "\"callsign\":\"%s\",\"dtmf_login\":\"%s\",\"ani\":\"%s\"}",
        new_id, username, display_name,
        (callsign_str && callsign_str[0]) ? callsign_str : "",
        dtmf_login, ani);

    free(username); free(name_str); free(email_str); free(callsign_str);
}

/* ── Admin status handler — appends sensitive fields to standard status ── */

static void handle_admin_api_status(struct mg_connection *c,
                                     struct mg_http_message *hm)
{
    /* Dispatch standard status first */
    kerchunk_resp_t resp;
    resp_init(&resp);
    const char *argv[2] = { "status", NULL };
    kerchunk_dispatch_command(1, argv, &resp);

    /* Append google_maps_api_key before closing brace */
    const kerchunk_config_t *cfg = kerchunk_core_get_config();
    const char *key = kerchunk_config_get(cfg, "general", "google_maps_api_key");
    if (key) {
        resp_str(&resp, "google_maps_api_key", key);
    }

    /* Audio stream fields — same as /api/status gets augmented with.
     * The admin dashboard's Live Audio card reads these to populate
     * Listeners / Sample rate / Bitrate; without them the stream-table
     * stays empty because handle_admin_api_status bypasses the inline
     * augmentation that handles the public route. */
    int sr = g_core ? g_core->sample_rate : 0;
    resp_int(&resp, "audio_listeners", atomic_load(&g_ws_audio_count));
    resp_int(&resp, "audio_sample_rate", sr);
    resp_int(&resp, "audio_bitrate_kbps", sr * 16 / 1000);

    resp_finish(&resp);
    mg_http_reply(c, 200, API_HEADERS, "%s", resp.json);
    (void)hm;
}

/* Forward decl: defined next to sse_publish_impl, below. Called from the
 * /api/events and /admin/api/events route handlers to replay cached
 * snapshots to a just-connected SSE client. */
static void emit_snapshot_burst(struct mg_connection *c, int admin);

/* ── Admin API dispatch — rewrites /admin/api/X to /api/X for handlers ── */

static void handle_admin_api(struct mg_connection *c,
                              struct mg_http_message *hm)
{
    /* /admin/api/status — special: includes sensitive fields */
    if (mg_match(hm->uri, mg_str("/admin/api/status"), NULL) &&
        mg_match(hm->method, mg_str("GET"), NULL)) {
        handle_admin_api_status(c, hm);
        return;
    }

    /* /admin/api/events — SSE */
    if (mg_match(hm->uri, mg_str("/admin/api/events"), NULL)) {
        mg_printf(c,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n");
        c->data[0] = 'E';  /* Mark as SSE client */
        atomic_fetch_add(&g_sse_count, 1);
        emit_snapshot_burst(c, 1);  /* replay cached snapshots */
        return;
    }

    /* /admin/api/config/reload */
    if (mg_match(hm->method, mg_str("POST"), NULL) &&
        mg_match(hm->uri, mg_str("/admin/api/config/reload"), NULL)) {
        kill(getpid(), SIGHUP);
        mg_http_reply(c, 200, API_HEADERS,
                      "{\"ok\":true,\"action\":\"config_reload\"}");
        return;
    }

    /* /admin/api/config */
    if (mg_match(hm->uri, mg_str("/admin/api/config"), NULL)) {
        if (mg_match(hm->method, mg_str("GET"), NULL))
            handle_api_config_get(c, hm);
        else if (mg_match(hm->method, mg_str("PUT"), NULL))
            handle_api_config_put(c, hm);
        else
            mg_http_reply(c, 405, API_HEADERS,
                          "{\"error\":\"Method not allowed\"}");
        return;
    }

    /* /admin/api/bulletin — GET current text, PUT replaces it */
    if (mg_match(hm->uri, mg_str("/admin/api/bulletin"), NULL)) {
        if (mg_match(hm->method, mg_str("GET"), NULL))
            handle_api_bulletin_get(c);
        else if (mg_match(hm->method, mg_str("PUT"), NULL))
            handle_api_bulletin_put(c, hm);
        else
            mg_http_reply(c, 405, API_HEADERS,
                          "{\"error\":\"Method not allowed\"}");
        return;
    }

    /* /admin/api/coverage-png — PUT the composed coverage map image */
    if (mg_match(hm->uri, mg_str("/admin/api/coverage-png"), NULL)) {
        if (mg_match(hm->method, mg_str("PUT"), NULL))
            handle_api_coverage_png_put(c, hm);
        else
            mg_http_reply(c, 405, API_HEADERS,
                          "{\"error\":\"Method not allowed\"}");
        return;
    }

    /* /admin/api/cmd */
    if (mg_match(hm->method, mg_str("POST"), NULL) &&
        mg_match(hm->uri, mg_str("/admin/api/cmd"), NULL)) {
        handle_api_post_cmd(c, hm);
        return;
    }

    /* /admin/api/users CRUD */
    if (mg_match(hm->method, mg_str("POST"), NULL) &&
        mg_match(hm->uri, mg_str("/admin/api/users"), NULL)) {
        handle_api_user_create(c, hm);
        return;
    }
    if (mg_match(hm->uri, mg_str("/admin/api/users/#"), NULL)) {
        /* Rewrite URI for uri_id() which expects /api/users/ prefix */
        struct mg_http_message rewritten = *hm;
        rewritten.uri.buf = hm->uri.buf + 6;   /* skip "/admin" */
        rewritten.uri.len = hm->uri.len - 6;
        if (mg_match(hm->method, mg_str("PUT"), NULL))
            handle_api_user_update(c, &rewritten);
        else if (mg_match(hm->method, mg_str("DELETE"), NULL))
            handle_api_user_delete(c, &rewritten);
        else
            mg_http_reply(c, 405, API_HEADERS, "{\"error\":\"Method not allowed\"}");
        return;
    }

    /* /admin/api/groups CRUD */
    if (mg_match(hm->method, mg_str("POST"), NULL) &&
        mg_match(hm->uri, mg_str("/admin/api/groups"), NULL)) {
        handle_api_group_create(c, hm);
        return;
    }
    if (mg_match(hm->uri, mg_str("/admin/api/groups/#"), NULL)) {
        struct mg_http_message rewritten = *hm;
        rewritten.uri.buf = hm->uri.buf + 6;
        rewritten.uri.len = hm->uri.len - 6;
        if (mg_match(hm->method, mg_str("PUT"), NULL))
            handle_api_group_update(c, &rewritten);
        else if (mg_match(hm->method, mg_str("DELETE"), NULL))
            handle_api_group_delete(c, &rewritten);
        else
            mg_http_reply(c, 405, API_HEADERS, "{\"error\":\"Method not allowed\"}");
        return;
    }
    if (mg_match(hm->uri, mg_str("/admin/api/groups"), NULL)) {
        handle_api_groups(c);
        return;
    }

    /* /admin/api/users */
    if (mg_match(hm->uri, mg_str("/admin/api/users"), NULL)) {
        handle_api_users(c);
        return;
    }

    /* /admin/api/commands */
    if (mg_match(hm->uri, mg_str("/admin/api/commands"), NULL)) {
        handle_api_commands(c);
        return;
    }

    /* /admin/api/audio — WebSocket with full PTT support */
    if (mg_match(hm->uri, mg_str("/admin/api/audio"), NULL)) {
        c->data[1] = 'A';  /* Mark as admin WebSocket */
        mg_ws_upgrade(c, hm, NULL);
        return;
    }

    /* /admin/api/... -- dynamic dispatch (strip /admin prefix) */
    if (hm->uri.len > 11 && memcmp(hm->uri.buf, "/admin/api/", 11) == 0) {
        /* Create a rewritten message for the generic handler */
        struct mg_http_message rewritten = *hm;
        rewritten.uri.buf = hm->uri.buf + 6;   /* skip "/admin" */
        rewritten.uri.len = hm->uri.len - 6;
        handle_api_get(c, &rewritten);
        return;
    }

    mg_http_reply(c, 404, API_HEADERS, "{\"error\":\"Unknown admin API endpoint\"}");
}

/* ── Mongoose event handler ── */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_ACCEPT && g_tls_active) {
        struct mg_tls_opts opts = { .cert = g_cert_data, .key = g_key_data };
        mg_tls_init(c, &opts);
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* Access log emitted at access_log_done: below, after the
         * handler has written the response into c->send. We capture
         * the buffer offset so we can read the response status line
         * ("HTTP/1.x NNN ...") that gets written there. Every return
         * inside this block jumps to that label so the access log
         * fires for every response (200, 304, 403, 404, 5xx alike). */
        size_t resp_off = c->send.len;

        /* CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            mg_http_reply(c, 204, CORS_HEADERS, "");
            goto access_log_done;
        }

        /* ════════════════════════════════════════════════════════
         *  /admin/... routes -- ACL + Basic Auth
         * ════════════════════════════════════════════════════════ */
        if (mg_match(hm->uri, mg_str("/admin/#"), NULL) ||
            mg_match(hm->uri, mg_str("/admin"), NULL)) {

            /* ACL check — if admin_acl is configured and client IP
             * doesn't match, return a plain 404 with no hint that
             * the admin interface exists. */
            if (!check_admin_acl(c)) {
                mg_http_reply(c, 404, "Content-Type: text/plain\r\n",
                              "Not found\n");
                goto access_log_done;
            }

            /* public_only mode: all admin access blocked */
            if (g_public_only) {
                mg_http_reply(c, 403, API_HEADERS,
                              "{\"error\":\"Admin access disabled (public_only mode)\"}");
                goto access_log_done;
            }

            /* Check Basic Auth */
            if (!check_basic_auth(hm)) {
                send_basic_auth_required(c,
                    mg_match(hm->uri, mg_str("/admin/api/#"), NULL) ? 1 : 0);
                goto access_log_done;
            }

            /* /admin/api/... -- admin API endpoints */
            if (mg_match(hm->uri, mg_str("/admin/api/#"), NULL)) {
                handle_admin_api(c, hm);
                goto access_log_done;
            }

            /* /admin/... -- serve from static_dir/admin/ naturally.
             * /admin/ serves admin/index.html via mg_http_serve_dir. */
            if (g_static_dir[0]) {
                /* Defense-in-depth: reject path traversal before mongoose */
                if (memmem(hm->uri.buf, hm->uri.len, "..", 2) != NULL) {
                    mg_http_reply(c, 403, "", "Forbidden\n");
                    goto access_log_done;
                }
                /* Cache-Control: no-cache forces every request to
                 * revalidate, so a freshly-deployed HTML/JS/CSS shows up
                 * on the next reload instead of being served stale for
                 * hours from the browser's heuristic cache. 304s still
                 * work via If-Modified-Since, so bytes only move when
                 * the file actually changed. */
                struct mg_http_serve_opts opts = {
                    .root_dir      = g_static_dir,
                    .extra_headers = "Cache-Control: no-cache\r\n",
                };
                mg_http_serve_dir(c, hm, &opts);
            } else {
                mg_http_reply(c, 404, "", "Not found\n");
            }
            goto access_log_done;
        }

        /* ════════════════════════════════════════════════════════
         *  /api/... routes -- public endpoints (no auth)
         * ════════════════════════════════════════════════════════ */
        if (mg_match(hm->uri, mg_str("/api/#"), NULL)) {

            /* Public GET routes (status, weather, nws) */
            if (mg_match(hm->method, mg_str("GET"), NULL)) {
                /* /api/status — inject web module flags */
                if (mg_match(hm->uri, mg_str("/api/status"), NULL)) {
                    kerchunk_resp_t resp;
                    resp_init(&resp);
                    const char *argv[2] = { "status", NULL };
                    kerchunk_dispatch_command(1, argv, &resp);
                    resp_bool(&resp, "registration_enabled", g_registration_enabled);
                    /* Audio stream info — useful for the public live-audio card */
                    int sr = g_core ? g_core->sample_rate : 0;
                    resp_int(&resp, "audio_listeners", atomic_load(&g_ws_audio_count));
                    resp_int(&resp, "audio_sample_rate", sr);
                    resp_int(&resp, "audio_bitrate_kbps", sr * 16 / 1000);
                    resp_finish(&resp);
                    mg_http_reply(c, 200, API_HEADERS, "%s", resp.json);
                    goto access_log_done;
                }
                for (int i = 0; g_public_apis[i]; i++) {
                    if (mg_match(hm->uri, mg_str(g_public_apis[i]), NULL)) {
                        handle_api_get(c, hm);
                        goto access_log_done;
                    }
                }
            }

            /* Registration — public */
            if (mg_match(hm->method, mg_str("POST"), NULL) &&
                mg_match(hm->uri, mg_str("/api/register"), NULL)) {
                handle_api_register(c, hm);
                goto access_log_done;
            }

            /* WebSocket audio stream — public (listen-only, no PTT commands) */
            if (mg_match(hm->uri, mg_str("/api/audio"), NULL)) {
                mg_ws_upgrade(c, hm, NULL);
                goto access_log_done;
            }

            /* /api/bulletin — public GET (markdown text) */
            if (mg_match(hm->uri, mg_str("/api/bulletin"), NULL) &&
                mg_match(hm->method, mg_str("GET"), NULL)) {
                handle_api_bulletin_get(c);
                goto access_log_done;
            }

            /* /api/events — public SSE (state transitions only, no PII) */
            if (mg_match(hm->uri, mg_str("/api/events"), NULL)) {
                mg_printf(c,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "\r\n");
                c->data[0] = 'P';  /* Mark as public SSE client */
                atomic_fetch_add(&g_sse_count, 1);
                emit_snapshot_burst(c, 0);  /* replay public snapshots */
                goto access_log_done;
            }

            /* All other /api/... routes are not publicly accessible --
             * they must go through /admin/api/... */
            mg_http_reply(c, 404, API_HEADERS,
                          "{\"error\":\"Unknown API endpoint\"}");
            goto access_log_done;
        }

        /* /coverage.png — served from coverage_png_path (default
         * /var/lib/kerchunk/coverage.png) so the admin planner can
         * publish over it without needing write access to the
         * static_dir. If the file isn't there yet, fall through to
         * the static-dir serve below for backward compat with sites
         * that keep a coverage.png alongside the other web assets. */
        if (mg_match(hm->uri, mg_str("/coverage.png"), NULL) &&
            mg_match(hm->method, mg_str("GET"), NULL)) {
            struct stat st;
            if (stat(g_coverage_png_path, &st) == 0 && S_ISREG(st.st_mode)) {
                handle_api_coverage_png_get(c, hm);
                goto access_log_done;
            }
            /* fall through to static serve */
        }

        /* ════════════════════════════════════════════════════════
         *  / — Public static files (no auth)
         * ════════════════════════════════════════════════════════ */
        if (g_static_dir[0]) {
            /* Defense-in-depth: reject path traversal before mongoose */
            if (memmem(hm->uri.buf, hm->uri.len, "..", 2) != NULL) {
                mg_http_reply(c, 403, "", "Forbidden\n");
                goto access_log_done;
            }
            /* See comment at the admin static-serve site; same rationale. */
            struct mg_http_serve_opts opts = {
                .root_dir      = g_static_dir,
                .extra_headers = "Cache-Control: no-cache\r\n",
            };
            mg_http_serve_dir(c, hm, &opts);
        } else {
            mg_http_reply(c, 404, "", "Not found\n");
        }

        access_log_done: {
            /* Parse status from "HTTP/1.x NNN " in c->send.buf at the
             * offset captured before the handler ran. Bytes-written =
             * total response size (headers + body). status=0 if we
             * couldn't parse — handler may have closed the connection
             * without writing a response. */
            int status = 0;
            size_t resp_len = c->send.len > resp_off ? c->send.len - resp_off : 0;
            if (resp_len >= 12) {
                const unsigned char *r = c->send.buf + resp_off;
                if (r[0]=='H' && r[1]=='T' && r[2]=='T' && r[3]=='P' && r[4]=='/' &&
                    r[9]>='0' && r[9]<='9' &&
                    r[10]>='0' && r[10]<='9' &&
                    r[11]>='0' && r[11]<='9')
                    status = (r[9]-'0')*100 + (r[10]-'0')*10 + (r[11]-'0');
            }

            uint8_t *ip = c->rem.addr.ip;
            if (c->rem.is_ip6)
                g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "%x:%x:%x:%x:%x:%x:%x:%x %.*s %.*s %d %zu",
                    (ip[0]<<8)|ip[1], (ip[2]<<8)|ip[3],
                    (ip[4]<<8)|ip[5], (ip[6]<<8)|ip[7],
                    (ip[8]<<8)|ip[9], (ip[10]<<8)|ip[11],
                    (ip[12]<<8)|ip[13], (ip[14]<<8)|ip[15],
                    (int)hm->method.len, hm->method.buf,
                    (int)hm->uri.len, hm->uri.buf,
                    status, resp_len);
            else
                g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "%d.%d.%d.%d %.*s %.*s %d %zu",
                    ip[0], ip[1], ip[2], ip[3],
                    (int)hm->method.len, hm->method.buf,
                    (int)hm->uri.len, hm->uri.buf,
                    status, resp_len);
        }
    }

    /* WebSocket audio — connection opened */
    if (ev == MG_EV_WS_OPEN) {
        ws_conn_state_t *st = calloc(1, sizeof(ws_conn_state_t));
        if (st) {
            st->magic = WS_PTT_MAGIC;
            st->admin = (c->data[1] == 'A') ? 1 : 0;
            /* Admin WS: mark authenticated (HTTP Basic Auth already passed).
             * user_id/user_name set later if WS auth command is sent (PTT page). */
            if (st->admin)
                st->authenticated = 1;
            ws_set_state(c, st);
        }
        int prev = atomic_fetch_add(&g_ws_audio_count, 1);
        if (prev == 0) {
            g_core->audio_tap_register(ws_rx_audio_tap, NULL);
            g_core->playback_tap_register(ws_tx_playback_tap, NULL);
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "audio streaming started");
        }
        /* Listener count just changed — republish the status snapshot
         * so every SSE-connected dashboard sees the new count live. */
        publish_status_snapshot();
    }

    /* WebSocket message (text commands or binary audio) */
    if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        ws_conn_state_t *st = ws_get_state(c);
        if (!st) return;

        uint8_t opcode = wm->flags & 0x0F;

        if (opcode == WEBSOCKET_OP_TEXT) {
            /* Public WebSocket is listen-only — reject all commands */
            if (!st->admin) {
                ws_send_json(c, "{\"ok\":false,\"error\":\"listen only\"}");
            } else {
                char *cmd = mg_json_get_str(wm->data, "$.cmd");
                if (cmd) {
                    if (strcmp(cmd, "auth") == 0)
                        ws_handle_auth(c, st, wm);
                    else if (strcmp(cmd, "ptt_on") == 0)
                        ws_handle_ptt_on(c, st);
                    else if (strcmp(cmd, "ptt_off") == 0)
                        ws_handle_ptt_off(c, st);
                    else
                        ws_send_json(c, "{\"ok\":false,\"error\":\"unknown cmd\"}");
                    free(cmd);
                }
            }
        } else if (opcode == WEBSOCKET_OP_BINARY) {
            /* Public WebSocket cannot send audio */
            if (!st->admin) return;
            ws_handle_audio_frame(st, (const uint8_t *)wm->data.buf,
                                  wm->data.len);
        }
    }

    /* Cross-thread wakeup: SSE events or audio flush */
    if (ev == MG_EV_WAKEUP) {
        struct mg_str *data = (struct mg_str *)ev_data;
        if (data->len == 1 && data->buf[0] == 'A') {
            /* Audio flush requested by audio_ws_thread */
            ws_flush_ring();
        } else if (data->len > 1) {
            /* SSE event broadcast. First byte is audience scope:
             *   'P' → send to both public ('P') and admin ('E') clients
             *   'E' → admin ('E') only */
            char scope = data->buf[0];
            const char *payload = data->buf + 1;
            int plen = (int)(data->len - 1);
            for (struct mg_connection *t = c->mgr->conns; t != NULL; t = t->next) {
                char tag = t->data[0];
                if (tag == 'E' || (tag == 'P' && scope == 'P'))
                    mg_printf(t, "data: %.*s\n\n", plen, payload);
            }
        }
    }

    /* Track client disconnections */
    if (ev == MG_EV_CLOSE) {
        if (c->data[0] == 'E' || c->data[0] == 'P') {
            atomic_fetch_sub(&g_sse_count, 1);
        } else {
            ws_conn_state_t *st = ws_get_state(c);
            if (st) {
                if (st->ptt_held) {
                    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                                "WS disconnect while PTT held: user=%s",
                                st->user_name);
                    st->ptt_held = 0;
                    if (g_ptt_holder == st) g_ptt_holder = NULL;
                    /* Queue drains naturally; PTT releases when empty */
                }
                free(st);
                ws_conn_state_t *nil = NULL;
                ws_set_state(c, nil);

                int prev = atomic_fetch_sub(&g_ws_audio_count, 1);
                if (prev == 1) {
                    g_core->audio_tap_unregister(ws_rx_audio_tap);
                    g_core->playback_tap_unregister(ws_tx_playback_tap);
                    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                                "audio streaming stopped");
                }
                /* Listener count changed — republish snapshot for the
                 * live "Listeners" counter on the dashboards. */
                publish_status_snapshot();
            }
        }
    }
}

/* ── Event handler for SSE broadcast ── */

/* Events safe to broadcast to unauthenticated public SSE clients.
 * Anything carrying PII (caller IDs, DTMF digits, recording paths) or
 * internal-only state (config reloads, queue preemption details) stays
 * on the admin-only channel. */
static int is_public_safe_event(kerchevt_type_t t)
{
    switch (t) {
    case KERCHEVT_COR_ASSERT:
    case KERCHEVT_COR_DROP:
    case KERCHEVT_VCOR_ASSERT:
    case KERCHEVT_VCOR_DROP:
    case KERCHEVT_PTT_ASSERT:
    case KERCHEVT_PTT_DROP:
    case KERCHEVT_RX_STATE_CHANGE:
    case KERCHEVT_TX_STATE_CHANGE:
    case KERCHEVT_TAIL_START:
    case KERCHEVT_TAIL_EXPIRE:
    case KERCHEVT_RX_TIMEOUT:
    case KERCHEVT_QUEUE_DRAIN:
    case KERCHEVT_QUEUE_COMPLETE:
    case KERCHEVT_ANNOUNCEMENT:
    case KERCHEVT_SHUTDOWN:
        return 1;
    default:
        return 0;
    }
}

/* ── SSE broadcast API for large/custom payloads ──
 *
 * Any module can call g_core->sse_publish("type", json, admin_only) to
 * open or update a named SSE channel. Every call:
 *   1. Pre-renders the full SSE data JSON.
 *   2. Broadcasts to every live SSE client (via the existing wakeup path).
 *   3. Caches the rendered line keyed by type so future connectors get a
 *      replay burst on connect and never need to HTTP-poll for initial
 *      state.
 *
 * The cache grows dynamically — there is no registration step and no
 * fixed limit on channel count. Re-publishing the same type replaces
 * the cached value. */

typedef struct {
    char   type[48];
    char  *line;        /* malloc'd JSON "{\"type\":...,\"data\":...,\"ts\":...}" */
    size_t line_len;
    int    admin_only;
} sse_snapshot_t;

static sse_snapshot_t *g_sse_snaps;
static int             g_sse_snap_count;
static int             g_sse_snap_cap;
static pthread_mutex_t g_sse_snap_mutex = PTHREAD_MUTEX_INITIALIZER;

static void sse_publish_impl(const char *type,
                              const char *payload_json,
                              int admin_only)
{
    if (!type || !*type || !payload_json) return;

    /* Render the full SSE data line once. Used for both the live broadcast
     * and the per-type snapshot cache. */
    size_t plen   = strlen(payload_json);
    size_t buflen = strlen(type) + plen + 64;
    char  *line   = malloc(buflen);
    if (!line) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_us = (uint64_t)ts.tv_sec * 1000000ULL +
                      (uint64_t)ts.tv_nsec / 1000ULL;

    int n = snprintf(line, buflen,
                     "{\"type\":\"%s\",\"data\":%s,\"ts\":%llu}",
                     type, payload_json, (unsigned long long)now_us);
    if (n < 0 || (size_t)n >= buflen) { free(line); return; }

    /* Replace existing slot with same type, or append a new one. */
    pthread_mutex_lock(&g_sse_snap_mutex);
    sse_snapshot_t *slot = NULL;
    for (int i = 0; i < g_sse_snap_count; i++) {
        if (strcmp(g_sse_snaps[i].type, type) == 0) { slot = &g_sse_snaps[i]; break; }
    }
    if (!slot) {
        if (g_sse_snap_count >= g_sse_snap_cap) {
            int new_cap = g_sse_snap_cap ? g_sse_snap_cap * 2 : 8;
            sse_snapshot_t *r = realloc(g_sse_snaps,
                                        (size_t)new_cap * sizeof(*g_sse_snaps));
            if (!r) { pthread_mutex_unlock(&g_sse_snap_mutex); free(line); return; }
            g_sse_snaps    = r;
            g_sse_snap_cap = new_cap;
        }
        slot = &g_sse_snaps[g_sse_snap_count++];
        snprintf(slot->type, sizeof(slot->type), "%s", type);
        slot->line = NULL;
    }
    free(slot->line);
    slot->line       = line;                /* cache takes ownership */
    slot->line_len   = (size_t)n;
    slot->admin_only = admin_only ? 1 : 0;
    pthread_mutex_unlock(&g_sse_snap_mutex);

    /* Broadcast to live clients. Uses the same scope-prefix protocol as
     * web_event_handler: 'P' = public+admin, 'E' = admin-only. We need
     * our own heap buffer here because the 520 B stack buffer in
     * web_event_handler is too small for large snapshots. */
    if (atomic_load(&g_sse_count) <= 0) return;

    char *wbuf = malloc((size_t)n + 2);
    if (!wbuf) return;
    wbuf[0] = admin_only ? 'E' : 'P';
    memcpy(wbuf + 1, line, (size_t)n);
    mg_wakeup(&g_mgr, g_listener_id, wbuf, (size_t)n + 1);
    free(wbuf);
}

/* Replay cached snapshots to a just-connected SSE client. Runs on the
 * mongoose thread synchronously after the HTTP headers are written, so
 * the client's very first network activity after connect is the current
 * snapshot burst. */
static void emit_snapshot_burst(struct mg_connection *c, int admin)
{
    pthread_mutex_lock(&g_sse_snap_mutex);
    for (int i = 0; i < g_sse_snap_count; i++) {
        sse_snapshot_t *s = &g_sse_snaps[i];
        if (s->admin_only && !admin) continue;
        if (!s->line || s->line_len == 0) continue;
        mg_printf(c, "data: %.*s\n\n", (int)s->line_len, s->line);
    }
    pthread_mutex_unlock(&g_sse_snap_mutex);
}

/* Build and publish the current status snapshot.
 *
 * Every field the dashboard needs to render "what is the repeater
 * doing right now" lives here. Published via sse_publish_impl under
 * the "status" type; that function caches the latest payload per
 * type so new SSE clients always see the current picture on connect
 * via emit_snapshot_burst() — no /api/status poll needed.
 *
 * Safe to call from any event-handler thread (sse_publish_impl takes
 * its own mutex). Rate-limiting isn't needed because we only call
 * on state-affecting events, not per-tick.
 *
 * Public-safe: nothing in the payload is admin-sensitive. */
static void publish_status_snapshot(void)
{
    if (!g_core || !g_core->sse_publish) return;

    const char *rx = kerchunk_get_rx_state();
    const char *tx = kerchunk_get_tx_state();
    int cor = g_core->is_receiving ? g_core->is_receiving() : 0;
    int ptt = kerchunk_core_get_ptt();
    int q   = g_core->queue_depth ? g_core->queue_depth() : 0;
    int em  = kerchunk_core_get_emergency();
    long long em_exp = (long long)kerchunk_core_get_emergency_expires_at();
    int listeners = atomic_load(&g_ws_audio_count);
    int sr = g_core->sample_rate;

    char payload[480];
    int n = snprintf(payload, sizeof(payload),
        "{\"rx_state\":\"%s\",\"tx_state\":\"%s\","
        "\"cor\":%s,\"ptt\":%s,"
        "\"emergency\":%s,\"emergency_expires_at\":%lld,"
        "\"queue_depth\":%d,\"audio_listeners\":%d,"
        "\"audio_sample_rate\":%d,\"audio_bitrate_kbps\":%d,"
        "\"registration_enabled\":%s}",
        rx ? rx : "IDLE",
        tx ? tx : "TX_IDLE",
        cor ? "true" : "false",
        ptt ? "true" : "false",
        em  ? "true" : "false",
        em_exp,
        q, listeners,
        sr, sr * 16 / 1000,
        g_registration_enabled ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(payload)) return;

    g_core->sse_publish("status", payload, 0 /* public-safe */);
}

static void web_event_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (evt->type == KERCHEVT_AUDIO_FRAME || evt->type == KERCHEVT_TICK ||
        evt->type == KERCHEVT_CTCSS_DETECT || evt->type == KERCHEVT_DCS_DETECT)
        return;

    /* Any event that might have changed the visible status image
     * triggers a snapshot republish. Done BEFORE the g_sse_count
     * short-circuit because sse_publish_impl caches the payload
     * unconditionally — so the first SSE client to connect later
     * still gets the current snapshot in their emit_snapshot_burst. */
    switch (evt->type) {
    case KERCHEVT_COR_ASSERT:
    case KERCHEVT_COR_DROP:
    case KERCHEVT_PTT_ASSERT:
    case KERCHEVT_PTT_DROP:
    case KERCHEVT_RX_STATE_CHANGE:
    case KERCHEVT_TX_STATE_CHANGE:
    case KERCHEVT_QUEUE_DRAIN:
    case KERCHEVT_QUEUE_COMPLETE:
    case KERCHEVT_QUEUE_PREEMPTED:
    case KERCHEVT_RX_TIMEOUT:
        publish_status_snapshot();
        break;
    case KERCHEVT_ANNOUNCEMENT:
        /* Refresh the snapshot when emergency mode flips so the
         * dashboard's emergency_expires_at field tracks the active
         * timer. Other announcement sources don't change the
         * status image. */
        if (evt->announcement.source &&
            strcmp(evt->announcement.source, "emergency") == 0)
            publish_status_snapshot();
        break;
    default:
        break;
    }

    if (atomic_load(&g_sse_count) <= 0)
        return;

    /* First byte of wakeup payload is the audience scope:
     *   'P' — broadcast to both public ('P') and admin ('E') SSE clients
     *   'E' — admin-only
     * Audio flush uses a 1-byte "A" message and is disambiguated by length. */
    char buf[520];
    buf[0] = is_public_safe_event(evt->type) ? 'P' : 'E';
    int jlen = kerchevt_to_json(evt, buf + 1, sizeof(buf) - 1);
    if (jlen > 0)
        mg_wakeup(&g_mgr, g_listener_id, buf, (size_t)jlen + 1);
}

/* ── Server threads ── */

/* Audio flush thread — wakes the mongoose thread on a tight 5ms cadence
 * to flush the SPSC ring to WebSocket clients.  Decoupled from mongoose
 * poll so TLS handshakes and HTTP processing can't stall audio delivery.
 *
 * mg_ws_send() is not thread-safe, so we use mg_wakeup() (which IS
 * thread-safe) to wake the mongoose poll.  The mongoose event handler
 * sees MG_EV_WAKEUP with data "A" and calls ws_flush_ring() on its
 * own thread. */
static int g_audio_ws_tid = -1;

static void *audio_ws_thread(void *arg)
{
    (void)arg;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    while (!g_core->thread_should_stop(g_audio_ws_tid)) {
        deadline.tv_nsec += 5000000L;  /* 5ms cadence */
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        /* Only wake mongoose when there are audio frames to flush */
        if (atomic_load(&g_ws_audio_count) > 0) {
            unsigned r = atomic_load_explicit(&g_ws_ring_r, memory_order_relaxed);
            unsigned w = atomic_load_explicit(&g_ws_ring_w, memory_order_acquire);
            if (r != w)
                mg_wakeup(&g_mgr, g_listener_id, "A", 1);
        }

        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }
    return NULL;
}

/* HTTP/WebSocket thread — handles all mongoose I/O on a single thread.
 * Audio flush is triggered by MG_EV_WAKEUP from the audio flush thread. */
static void *web_thread(void *arg)
{
    (void)arg;
    while (!g_core->thread_should_stop(g_web_tid)) {
        mg_mgr_poll(&g_mgr, 50);  /* 50ms poll — mg_wakeup interrupts for audio */
    }
    return NULL;
}

/* ── Module lifecycle ── */

static int web_load(kerchunk_core_t *core)
{
    g_core = core;
    build_cors_headers();
    /* Expose the SSE broadcast API to other modules. */
    core->sse_publish = sse_publish_impl;
    return 0;
}

static int web_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "web", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    g_port = kerchunk_config_get_int(cfg, "web", "port", 8080);

    v = kerchunk_config_get(cfg, "web", "bind");
    if (v) snprintf(g_bind, sizeof(g_bind), "%s", v);

    v = kerchunk_config_get(cfg, "web", "auth_user");
    if (v) snprintf(g_auth_user, sizeof(g_auth_user), "%s", v);

    v = kerchunk_config_get(cfg, "web", "auth_password");
    if (v) snprintf(g_auth_password, sizeof(g_auth_password), "%s", v);



    v = kerchunk_config_get(cfg, "web", "public_only");
    g_public_only = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "web", "admin_acl");
    parse_admin_acl(v);
    if (g_admin_acl_count > 0)
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "admin ACL: %d entries (non-matching IPs get 404)", g_admin_acl_count);

    v = kerchunk_config_get(cfg, "web", "static_dir");
    if (v) snprintf(g_static_dir, sizeof(g_static_dir), "%s", v);

    v = kerchunk_config_get(cfg, "web", "tls_cert");
    if (v) snprintf(g_tls_cert, sizeof(g_tls_cert), "%s", v);

    v = kerchunk_config_get(cfg, "web", "tls_key");
    if (v) snprintf(g_tls_key, sizeof(g_tls_key), "%s", v);

    v = kerchunk_config_get(cfg, "web", "cors_origin");
    if (v) snprintf(g_cors_origin, sizeof(g_cors_origin) - 1, "%s", v);
    build_cors_headers();

    if (strcmp(g_cors_origin, "*") == 0 &&
        strcmp(g_bind, "127.0.0.1") != 0 &&
        strcmp(g_bind, "::1") != 0 &&
        strcmp(g_bind, "localhost") != 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "WARNING: CORS origin is wildcard (*) on non-localhost bind (%s) — "
                    "consider setting cors_origin to a specific origin", g_bind);
    }

    v = kerchunk_config_get(cfg, "web", "registration_enabled");
    g_registration_enabled = (v && strcmp(v, "on") == 0);

    if (g_registration_enabled && !g_tls_active &&
        strcmp(g_bind, "127.0.0.1") != 0 && strcmp(g_bind, "::1") != 0 &&
        strcmp(g_bind, "localhost") != 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "WARNING: registration enabled without TLS on %s — "
                    "credentials will be sent in plaintext", g_bind);
    }

    v = kerchunk_config_get(cfg, "web", "ptt_enabled");
    g_ptt_enabled = (v && strcmp(v, "on") == 0);

    g_ptt_max_duration_s = kerchunk_config_get_duration_s(cfg, "web", "ptt_max_duration", 30);
    g_ptt_priority = kerchunk_config_get_int(cfg, "web", "ptt_priority", KERCHUNK_PRI_NORMAL);

    v = kerchunk_config_get(cfg, "bulletin", "path");
    if (v && *v) snprintf(g_bulletin_path, sizeof(g_bulletin_path), "%s", v);
    g_bulletin_max_size = kerchunk_config_get_int(cfg, "bulletin", "max_size",
                                                   g_bulletin_max_size);
    if (g_bulletin_max_size < 256) g_bulletin_max_size = 256;
    if (g_bulletin_max_size > (1 << 20)) g_bulletin_max_size = 1 << 20;

    v = kerchunk_config_get(cfg, "web", "coverage_png_path");
    if (v && *v) snprintf(g_coverage_png_path, sizeof(g_coverage_png_path), "%s", v);
    g_coverage_png_max_size = kerchunk_config_get_int(cfg, "web",
                                                       "coverage_png_max_size",
                                                       g_coverage_png_max_size);
    if (g_coverage_png_max_size < 4096) g_coverage_png_max_size = 4096;
    if (g_coverage_png_max_size > 32 * 1024 * 1024)
        g_coverage_png_max_size = 32 * 1024 * 1024;

    /* Seed the SSE snapshot cache with whatever is on disk right now so
     * first-time visitors see the bulletin as soon as they connect —
     * otherwise the cache stays empty until the next admin edit. */
    publish_bulletin_snapshot();

    /* Redirect mongoose logs through our logger */
    mg_log_set_fn(mg_log_cb, NULL);
    mg_log_set(MG_LL_DEBUG);

    /* If already running, just update config values (auth_password, static_dir)
     * without restarting — restarting would deadlock if triggered by a web
     * API handler on the mongoose thread via SIGHUP. */
    if (g_web_tid >= 0) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "config reloaded (port=%d auth=%s public_only=%s)",
                    g_port, g_auth_password[0] ? "yes" : "no",
                    g_public_only ? "yes" : "no");
        return 0;
    }

    if (!g_enabled) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled");
        return 0;
    }

    if (g_cert_data.buf) { free(g_cert_data.buf); g_cert_data.buf = NULL; }
    if (g_key_data.buf) { free(g_key_data.buf); g_key_data.buf = NULL; }

    /* TLS setup — read cert/key files once */
    g_tls_active = 0;
    if (g_tls_cert[0] && g_tls_key[0]) {
        g_cert_data = mg_file_read(&mg_fs_posix, g_tls_cert);
        if (g_cert_data.buf == NULL) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "failed to read TLS cert: %s", g_tls_cert);
            g_enabled = 0;
            return 0;
        }
        g_key_data = mg_file_read(&mg_fs_posix, g_tls_key);
        if (g_key_data.buf == NULL) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "failed to read TLS key: %s", g_tls_key);
            free(g_cert_data.buf);
            g_cert_data.buf = NULL;
            g_enabled = 0;
            return 0;
        }
        g_tls_active = 1;
    }

    /* Initialize mongoose */
    mg_mgr_init(&g_mgr);
    mg_wakeup_init(&g_mgr);

    /* Make wakeup pipe non-blocking so event producers (audio thread,
     * main thread) never block if the pipe buffer is full. Mongoose's
     * MSG_NONBLOCKING is defined as 0 on Linux, so send() would block. */
    if (g_mgr.pipe != MG_INVALID_SOCKET)
        fcntl((int)(size_t)g_mgr.pipe, F_SETFL,
              fcntl((int)(size_t)g_mgr.pipe, F_GETFL) | O_NONBLOCK);

    char url[128];
    /* IPv6 addresses need brackets in URLs: http://[::]:8080 */
    if (strchr(g_bind, ':'))
        snprintf(url, sizeof(url), "%s://[%s]:%d",
                 g_tls_active ? "https" : "http", g_bind, g_port);
    else
        snprintf(url, sizeof(url), "%s://%s:%d",
                 g_tls_active ? "https" : "http", g_bind, g_port);

    struct mg_connection *listener = mg_http_listen(&g_mgr, url,
                                                     ev_handler, NULL);
    if (!listener) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "failed to listen on %s", url);
        mg_mgr_free(&g_mgr);
        if (g_cert_data.buf) { free(g_cert_data.buf); g_cert_data.buf = NULL; }
        if (g_key_data.buf) { free(g_key_data.buf); g_key_data.buf = NULL; }
        g_enabled = 0;
        return 0;
    }
    g_listener_id = listener->id;

    /* Subscribe to events for SSE broadcast (unsubscribe first to avoid
     * duplicates on config reload — configure() is called on every SIGHUP) */
    static const kerchevt_type_t types[] = {
        KERCHEVT_COR_ASSERT, KERCHEVT_COR_DROP, KERCHEVT_VCOR_ASSERT, KERCHEVT_VCOR_DROP,
        KERCHEVT_PTT_ASSERT, KERCHEVT_PTT_DROP,
        KERCHEVT_RX_STATE_CHANGE, KERCHEVT_TX_STATE_CHANGE,
        KERCHEVT_TAIL_START, KERCHEVT_TAIL_EXPIRE, KERCHEVT_RX_TIMEOUT,
        KERCHEVT_CALLER_IDENTIFIED, KERCHEVT_CALLER_CLEARED,
        KERCHEVT_DTMF_DIGIT, KERCHEVT_DTMF_END,
        KERCHEVT_QUEUE_DRAIN, KERCHEVT_QUEUE_COMPLETE, KERCHEVT_QUEUE_PREEMPTED,
        KERCHEVT_RECORDING_SAVED, KERCHEVT_ANNOUNCEMENT,
        KERCHEVT_CONFIG_RELOAD, KERCHEVT_SHUTDOWN, KERCHEVT_HEARTBEAT,
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        g_core->unsubscribe(types[i], web_event_handler);
    for (int i = 0; i <= 15; i++)
        g_core->unsubscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i), web_event_handler);

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        g_core->subscribe(types[i], web_event_handler, NULL);
    for (int i = 0; i <= 15; i++)
        g_core->subscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i),
                           web_event_handler, NULL);

    /* Start server thread */
    g_sse_count = 0;
    g_web_tid = g_core->thread_create("web-server", web_thread, NULL);
    if (g_web_tid < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create web thread");
        mg_mgr_free(&g_mgr);
        g_enabled = 0;
        return 0;
    }

    /* Start audio flush thread — wakes mongoose for WebSocket audio */
    g_audio_ws_tid = g_core->thread_create("web-audio", audio_ws_thread, NULL);
    if (g_audio_ws_tid < 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "audio flush thread failed — WebSocket audio may stutter");
    }

    if (!g_tls_active &&
        strcmp(g_bind, "127.0.0.1") != 0 &&
        strcmp(g_bind, "::1") != 0 &&
        strcmp(g_bind, "localhost") != 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "WARNING: listening on %s without TLS — traffic is unencrypted",
                    g_bind);
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "listening on %s://%s:%d%s%s",
                g_tls_active ? "https" : "http",
                g_bind, g_port,
                g_auth_password[0] ? " (auth required)" : "",
                g_public_only ? " (public only)" : "");

    /* Seed the "status" snapshot cache with the current state so the
     * very first SSE client gets the right picture on their
     * emit_snapshot_burst — no poll needed, no "everything is IDLE"
     * default until the first event fires. */
    publish_status_snapshot();

    return 0;
}

static void web_unload(void)
{
    /* Unsubscribe from all event types */
    static const kerchevt_type_t types[] = {
        KERCHEVT_COR_ASSERT, KERCHEVT_COR_DROP, KERCHEVT_VCOR_ASSERT, KERCHEVT_VCOR_DROP,
        KERCHEVT_PTT_ASSERT, KERCHEVT_PTT_DROP,
        KERCHEVT_RX_STATE_CHANGE, KERCHEVT_TX_STATE_CHANGE,
        KERCHEVT_TAIL_START, KERCHEVT_TAIL_EXPIRE, KERCHEVT_RX_TIMEOUT,
        KERCHEVT_CALLER_IDENTIFIED, KERCHEVT_CALLER_CLEARED,
        KERCHEVT_DTMF_DIGIT, KERCHEVT_DTMF_END,
        KERCHEVT_QUEUE_DRAIN, KERCHEVT_QUEUE_COMPLETE, KERCHEVT_QUEUE_PREEMPTED,
        KERCHEVT_RECORDING_SAVED, KERCHEVT_ANNOUNCEMENT,
        KERCHEVT_CONFIG_RELOAD, KERCHEVT_SHUTDOWN, KERCHEVT_HEARTBEAT,
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        g_core->unsubscribe(types[i], web_event_handler);
    for (int i = 0; i <= 15; i++)
        g_core->unsubscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i), web_event_handler);

    /* Stop audio flush thread first (it wakes mongoose via mg_wakeup) */
    if (g_audio_ws_tid >= 0) {
        g_core->thread_stop(g_audio_ws_tid);
        g_core->thread_join(g_audio_ws_tid);
        g_audio_ws_tid = -1;
    }

    /* Broadcast shutdown to all clients before stopping */
    if (g_web_tid >= 0) {
        for (struct mg_connection *c = g_mgr.conns; c; c = c->next) {
            if (c->data[0] == 'E') {
                /* SSE: send shutdown event */
                mg_printf(c, "data: {\"type\":\"shutdown\"}\n\n");
            } else if (c->is_websocket) {
                /* WebSocket: send close frame (1001 = going away) */
                mg_ws_send(c, "", 0, WEBSOCKET_OP_CLOSE);
            }
        }
        /* Give mongoose one poll cycle to flush the close frames */
        mg_mgr_poll(&g_mgr, 50);

        g_core->thread_stop(g_web_tid);
        g_core->thread_join(g_web_tid);
        g_web_tid = -1;
    }

    /* Release any WebSocket PTT that was held during shutdown.
     * NOTE: this runs on the main thread while the mongoose thread has
     * already been joined above, so there is no actual race here. */
    if (g_ptt_holder) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "shutdown: releasing orphaned WS PTT (user=%s)",
                    g_ptt_holder->user_name);
        g_ptt_holder->ptt_held = 0;
        g_ptt_holder = NULL;
    }

    /* Unregister audio taps explicitly in case WS close handlers
     * didn't fire cleanly during mg_mgr_free */
    if (atomic_load(&g_ws_audio_count) > 0) {
        g_core->audio_tap_unregister(ws_rx_audio_tap);
        g_core->playback_tap_unregister(ws_tx_playback_tap);
        atomic_store(&g_ws_audio_count, 0);
    }

    /* Close all connections and free the manager.
     * mg_mgr_free fires MG_EV_CLOSE for each connection, which frees
     * per-connection ws_conn_state_t allocations. */
    mg_mgr_free(&g_mgr);

    if (g_cert_data.buf) { free(g_cert_data.buf); g_cert_data.buf = NULL; }
    if (g_key_data.buf) { free(g_key_data.buf); g_key_data.buf = NULL; }

    /* Tear down the SSE snapshot cache. Safe to do without the mutex here
     * because the web thread has been joined and no more sse_publish_impl
     * calls can land — but hold the mutex anyway for symmetry. */
    pthread_mutex_lock(&g_sse_snap_mutex);
    for (int i = 0; i < g_sse_snap_count; i++)
        free(g_sse_snaps[i].line);
    free(g_sse_snaps);
    g_sse_snaps      = NULL;
    g_sse_snap_count = 0;
    g_sse_snap_cap   = 0;
    pthread_mutex_unlock(&g_sse_snap_mutex);

    g_core->sse_publish = NULL;
}

/* ── CLI ── */

static int cli_web(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    resp_bool(r, "enabled", g_enabled);
    resp_int(r, "port", g_port);
    resp_str(r, "bind", g_bind);
    resp_bool(r, "auth", g_auth_password[0] != '\0');
    resp_str(r, "auth_user", g_auth_user);
    resp_bool(r, "public_only", g_public_only);
    resp_bool(r, "tls", g_tls_active);
    if (g_tls_active)
        resp_str(r, "tls_cert", g_tls_cert);
    resp_int(r, "sse_clients", atomic_load(&g_sse_count));
    resp_int(r, "audio_clients", atomic_load(&g_ws_audio_count));
    resp_bool(r, "ptt_enabled", g_ptt_enabled);
    if (g_ptt_enabled) {
        resp_int(r, "ptt_max_duration", g_ptt_max_duration_s);
        resp_int(r, "ptt_priority", g_ptt_priority);
    }
    if (g_static_dir[0])
        resp_str(r, "static_dir", g_static_dir);
    return 0;

usage:
    resp_text_raw(r, "Embedded HTTP/HTTPS server for web dashboard\n\n"
        "  web\n"
        "    Show web server status, TLS state, and connected clients.\n\n"
        "    Fields:\n"
        "      enabled        Whether the web server is running\n"
        "      port           Listening port number\n"
        "      bind           Bind address (e.g. 127.0.0.1, 0.0.0.0)\n"
        "      auth           Whether auth_password is configured\n"
        "      tls            Whether TLS/HTTPS is active\n"
        "      sse_clients    Number of connected SSE event streams\n"
        "      audio_clients  Number of connected WebSocket audio streams\n"
        "      ptt_enabled    Whether web PTT is allowed\n"
        "      static_dir     Path to static file directory\n\n"
        "    Serves the web dashboard, JSON API, SSE live events,\n"
        "    and WebSocket audio streaming.\n\n"
        "Config: [web] enabled, port, bind, auth_password, static_dir,\n"
        "        tls_cert, tls_key, ptt_enabled, ptt_max_duration,\n"
        "        ptt_priority, registration_enabled\n");
    resp_str(r, "error", "usage: web [help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "web", .usage = "web", .description = "Web server status", .handler = cli_web, .category = "Control" },
};

static kerchunk_module_def_t mod_web = {
    .name             = "mod_web",
    .version          = "1.0.0",
    .description      = "Embedded HTTP/HTTPS server for web dashboard",
    .load             = web_load,
    .configure        = web_configure,
    .unload           = web_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_web);
