/*
 * mod_link.c — bridge to a kerchunk-reflectd network.
 *
 * Implements the client side of LINK-PROTOCOL.md. Connects to a
 * configured reflector over WebSocket (TLS optional via wss://), logs
 * in with HMAC-SHA256 over a server-issued challenge, and bridges:
 *
 *   - local RX audio (taken from the audio_tap during COR-active)
 *     → 48k → 24k resample → Opus 60 ms → SRTP → UDP → reflector
 *
 *   - reflector audio (UDP → SRTP unprotect → Opus decode → 24k → 48k)
 *     → kerchunk_queue_add_buffer_src + auto request_ptt
 *
 * Threading: a single background pthread owns the WS, UDP socket, Opus
 * codecs, SRTP contexts, jitter buffer, and the reconnect/backoff state
 * machine. The audio_tap callback (which fires on the audio thread)
 * only writes raw 48 kHz int16 frames into a single-producer-single-
 * consumer ring; the bg thread drains it. The bg thread feeds decoded
 * audio to the playback path via kerchunk_queue_add_buffer_src, which
 * is pthread-safe.
 *
 * No floor enforcement on the local side — that's the reflector's job.
 * mod_link just sends when local COR is up and plays whatever the
 * reflector sends back.
 */

#include "kerchunk.h"
#include "kerchunk_config.h"
#include "kerchunk_events.h"
#include "kerchunk_log.h"
#include "kerchunk_queue.h"
#include "kerchunk_wav.h"        /* kerchunk_resample_into */
#include "kerchunk_link_proto.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <opus/opus.h>
#include <srtp2/srtp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include "../vendor/mongoose.h"

#define LOG_MOD "link"

/* ── Globals ──────────────────────────────────────────────────────── */

static kerchunk_core_t *g_core;

/* Config (read in configure(); never written from bg thread).
 * g_enabled is volatile because the bg thread polls it every loop and
 * configure() writes it from the main thread — without volatile a
 * release build can hoist the read out of the loop. */
static volatile int  g_enabled;
static char g_node_id[64];
static char g_reflector_ws[256];
static uint8_t g_psk[KERCHUNK_LINK_PSK_BYTES];
static int  g_psk_loaded;
static int  g_default_tg;
static int  g_link_tail_ms;
static int  g_jb_target_ms;
static int  g_opus_bitrate;
static int  g_opus_loss_perc;
static int  g_reconnect_min_ms;
static int  g_reconnect_max_ms;
static int  g_verify_peer;

/* Background thread + lifecycle. */
static pthread_t        g_thread;
static volatile int     g_run;
static int              g_thread_started;
static struct mg_mgr    g_mgr;
static struct mg_connection *g_ws;        /* control-plane WS */

/* Connection state machine — feeds the dashboard SSE snapshot. */
typedef enum {
    LST_DISABLED, LST_CONNECTING, LST_AWAIT_HELLO, LST_AWAIT_LOGIN_OK,
    LST_CONNECTED, LST_RECONNECTING, LST_STOPPED,
} link_state_t;
static link_state_t g_state;
static int          g_reconnect_attempt;
static int64_t      g_next_reconnect_ms;
static int64_t      g_state_changed_ms;
static int64_t      g_session_started_at;   /* epoch s when LST_CONNECTED last entered; 0 if not connected */
static char         g_last_error[160];
static char         g_current_talker[64];
static int          g_current_tg;
static char         g_tg_members_json[512];   /* raw JSON array from login_ok */

/* SRTP / RTP / Opus session (set after login_ok, freed on disconnect). */
static int           g_session_active;
static srtp_t        g_srtp_in, g_srtp_out;
static OpusEncoder  *g_enc;
static OpusDecoder  *g_dec;
static uint32_t      g_node_ssrc, g_refl_ssrc;
static uint16_t      g_out_seq;
static uint32_t      g_out_ts;
static int           g_udp_fd = -1;
static struct sockaddr_in g_rtp_dst;

/* Counters for SSE snapshot. bytes_* count the wire bytes (RTP
 * header + ciphertext payload + SRTP auth tag) — what actually
 * crosses the socket — so they line up with reflectd's per-node
 * bytes_in / bytes_out figures. */
static int       g_pkts_sent;
static int       g_pkts_recv;
static uint64_t  g_bytes_sent;
static uint64_t  g_bytes_recv;
static int       g_srtp_auth_fail;
static int       g_decode_err;

/* SRTP-drift detection: a sliding 10s window. If we accumulate >1000
 * auth failures in any 10s span, force a reconnect to renegotiate the
 * master key (LINK-PROTOCOL.md § 4.6 row "SRTP auth failures > 1000 / 10s"). */
static int     g_srtp_window_count;
static int64_t g_srtp_window_start_ms;
#define SRTP_DRIFT_WINDOW_MS  10000
#define SRTP_DRIFT_THRESHOLD  1000

/* NAT keepalive cadence — sends a PT=99 marker (no floor grab) every
 * 10s when not transmitting so the upstream NAT mapping stays open. */
#define NAT_KEEPALIVE_MS 10000
static int64_t g_last_nat_keepalive_ms;

/* Server-driven mute (LINK-PROTOCOL.md § 4.6). When set, mod_link stops
 * encoding+sending entirely; control plane stays alive. */
static volatile int g_muted;

static void send_ws_json(const char *json);   /* fwd decl — defined below */

/* Per-stream RTP-seq tracking for accurate loss-rate calculation
 * (LINK-PROTOCOL.md § 4.1.4 quality report). Walks the seq, counts gaps. */
static uint16_t g_recv_last_seq;
static int      g_recv_have_seq;
static int      g_recv_lost;       /* in current 10s window */
static int      g_recv_received;   /* in current 10s window */
static int64_t  g_quality_window_start_ms;
#define QUALITY_REPORT_MS 10000

/* Adaptive bitrate (LINK-PROTOCOL.md § 4.1.4). Reflector can suggest a target
 * bitrate via the target_bitrate message; we update the encoder. */
static int g_current_bitrate;

/* PTT state (bg thread owns these). */
static int     g_link_ptt_held;
static int64_t g_link_last_recv_ms;

/* Cross-thread request flags. CLI / DTMF on the main thread sets one
 * of these; the bg thread polls and acts. mongoose mg_ws_send is not
 * thread-safe so we can't fire WS messages directly from CLI. */
static volatile int g_req_set_tg;     /* 0 = none, else target TG number */
static volatile int g_req_reconnect;
static volatile int g_req_clear_alarm;

/* Local RX state (used to decide whether to encode samples to send). */
static volatile int g_local_rx_active;

/* ── SPSC ring for raw 48 kHz int16 samples (audio thread → bg) ───── */

#define LINK_RING_SAMPLES (48000 * 2)   /* 2 seconds of 48 kHz audio */
static int16_t  g_ring[LINK_RING_SAMPLES];
static volatile uint32_t g_ring_w;
static volatile uint32_t g_ring_r;

static size_t ring_avail_read(void)
{
    uint32_t w = __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE);
    uint32_t r = __atomic_load_n(&g_ring_r, __ATOMIC_RELAXED);
    return (size_t)((w - r) % LINK_RING_SAMPLES);
}
static size_t ring_avail_write(void)
{
    return LINK_RING_SAMPLES - 1 - ring_avail_read();
}
static void ring_write(const int16_t *src, size_t n)
{
    uint32_t w = __atomic_load_n(&g_ring_w, __ATOMIC_RELAXED);
    for (size_t i = 0; i < n; i++) {
        g_ring[(w + i) % LINK_RING_SAMPLES] = src[i];
    }
    __atomic_store_n(&g_ring_w, (w + n) % LINK_RING_SAMPLES, __ATOMIC_RELEASE);
}
static size_t ring_read(int16_t *dst, size_t n)
{
    size_t avail = ring_avail_read();
    if (n > avail) n = avail;
    uint32_t r = __atomic_load_n(&g_ring_r, __ATOMIC_RELAXED);
    for (size_t i = 0; i < n; i++) {
        dst[i] = g_ring[(r + i) % LINK_RING_SAMPLES];
    }
    __atomic_store_n(&g_ring_r, (r + n) % LINK_RING_SAMPLES, __ATOMIC_RELEASE);
    return n;
}

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

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── State change + SSE snapshot ──────────────────────────────────── */

static const char *state_name(link_state_t s)
{
    switch (s) {
    case LST_DISABLED:        return "disabled";
    case LST_CONNECTING:      return "connecting";
    case LST_AWAIT_HELLO:     return "connecting";
    case LST_AWAIT_LOGIN_OK:  return "connecting";
    case LST_CONNECTED:       return "connected";
    case LST_RECONNECTING:    return "reconnecting";
    case LST_STOPPED:         return "stopped";
    }
    return "?";
}

static void publish_snapshot(void)
{
    if (!g_core || !g_core->sse_publish) return;
    char json[1024];
    int64_t now_s = (int64_t)time(NULL);
    long uptime_s = g_session_started_at
        ? (long)(now_s - g_session_started_at) : 0;
    snprintf(json, sizeof(json),
        "{\"type\":\"link\",\"state\":\"%s\",\"tg\":%d,"
        "\"current_talker\":%s%s%s,"
        "\"reconnect_attempt\":%d,\"last_error\":%s%s%s,"
        "\"muted\":%s,"
        "\"tg_members\":%s,"
        "\"session_started_at\":%lld,\"uptime_s\":%ld,"
        "\"counters\":{\"sent\":%d,\"recv\":%d,"
        "\"bytes_sent\":%llu,\"bytes_recv\":%llu,"
        "\"srtp_auth_fail\":%d,\"decode_err\":%d,"
        "\"loss_pct_x10\":%d,\"bitrate\":%d}}",
        state_name(g_state), g_current_tg,
        g_current_talker[0] ? "\"" : "", g_current_talker[0] ? g_current_talker : "null",
        g_current_talker[0] ? "\"" : "",
        g_reconnect_attempt,
        g_last_error[0] ? "\"" : "", g_last_error[0] ? g_last_error : "null",
        g_last_error[0] ? "\"" : "",
        g_muted ? "true" : "false",
        g_tg_members_json[0] ? g_tg_members_json : "[]",
        (long long)g_session_started_at, uptime_s,
        g_pkts_sent, g_pkts_recv,
        (unsigned long long)g_bytes_sent,
        (unsigned long long)g_bytes_recv,
        g_srtp_auth_fail, g_decode_err,
        g_recv_received + g_recv_lost > 0
            ? (g_recv_lost * 1000 / (g_recv_received + g_recv_lost)) : 0,
        g_current_bitrate);
    g_core->sse_publish("link", json, 0);
}

static void set_state(link_state_t s)
{
    if (g_state == s) return;
    /* Latch a session start when we first enter CONNECTED, clear on
     * any leave. Drives the dashboard's "Uptime" field — uses wall
     * time so it survives display in human-friendly units. */
    if (s == LST_CONNECTED && g_state != LST_CONNECTED)
        g_session_started_at = (int64_t)time(NULL);
    else if (s != LST_CONNECTED)
        g_session_started_at = 0;
    g_state = s;
    g_state_changed_ms = now_ms();
    publish_snapshot();
}

static void set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_error, sizeof(g_last_error), fmt, ap);
    va_end(ap);
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "%s", g_last_error);
}

/* ── Session teardown ─────────────────────────────────────────────── */

static void session_cleanup(void)
{
    if (g_link_ptt_held) {
        g_core->release_ptt("link");
        g_link_ptt_held = 0;
    }
    if (g_session_active) {
        srtp_dealloc(g_srtp_in);
        srtp_dealloc(g_srtp_out);
        g_session_active = 0;
    }
    if (g_enc) { opus_encoder_destroy(g_enc); g_enc = NULL; }
    if (g_dec) { opus_decoder_destroy(g_dec); g_dec = NULL; }
    if (g_udp_fd >= 0)  { close(g_udp_fd);  g_udp_fd  = -1; }
    g_out_seq = 0; g_out_ts = 0;
    g_current_talker[0] = '\0';
    g_current_tg = 0;
    /* Drain ring so we don't replay old audio after reconnect. */
    __atomic_store_n(&g_ring_r, __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
}

/* ── SRTP / Opus / UDP setup from login_ok ────────────────────────── */

static int session_setup(struct mg_str body)
{
    char *ep   = mg_json_get_str(body, "$.rtp_endpoint");
    char *kh   = mg_json_get_str(body, "$.srtp_master_key");
    char *sh   = mg_json_get_str(body, "$.srtp_master_salt");
    long  ns   = mg_json_get_long(body, "$.ssrc",            0);
    long  rs   = mg_json_get_long(body, "$.reflector_ssrc",  0);
    long  tg   = mg_json_get_long(body, "$.talkgroup",       0);
    int   rc   = -1;

    /* Stash tg_members verbatim for the SSE snapshot (page renders the
     * list directly). Falls back to "[]" if the reflector didn't send. */
    struct mg_str members = mg_json_get_tok(body, "$.tg_members");
    if (members.buf && members.len > 0 &&
        members.len < sizeof(g_tg_members_json)) {
        memcpy(g_tg_members_json, members.buf, members.len);
        g_tg_members_json[members.len] = '\0';
    } else {
        g_tg_members_json[0] = '\0';
    }

    if (!ep || !kh || !sh ||
        strlen(kh) != 2 * KERCHUNK_LINK_SRTP_KEY_BYTES ||
        strlen(sh) != 2 * KERCHUNK_LINK_SRTP_SALT_BYTES ||
        !ns || !rs || !tg) {
        set_error("login_ok missing or malformed audio fields");
        goto out;
    }
    g_node_ssrc  = (uint32_t)ns;
    g_refl_ssrc  = (uint32_t)rs;
    g_current_tg = (int)tg;

    /* Parse rtp_endpoint = "host:port". host can be IPv4 literal or DNS. */
    char *colon = strrchr(ep, ':');
    if (!colon) { set_error("rtp_endpoint not host:port"); goto out; }
    *colon = '\0';
    int port = atoi(colon + 1);
    memset(&g_rtp_dst, 0, sizeof(g_rtp_dst));
    g_rtp_dst.sin_family = AF_INET;
    g_rtp_dst.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, ep, &g_rtp_dst.sin_addr) != 1) {
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(ep, NULL, &hints, &res) != 0 || !res) {
            set_error("rtp_endpoint host '%s' did not resolve", ep);
            if (res) freeaddrinfo(res);
            goto out;
        }
        g_rtp_dst.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    /* Combined master = key||salt for libsrtp. */
    static uint8_t master[KERCHUNK_LINK_SRTP_KEY_BYTES +
                          KERCHUNK_LINK_SRTP_SALT_BYTES];
    if (hex_to_bytes(kh, master, KERCHUNK_LINK_SRTP_KEY_BYTES) != 0 ||
        hex_to_bytes(sh, master + KERCHUNK_LINK_SRTP_KEY_BYTES,
                     KERCHUNK_LINK_SRTP_SALT_BYTES) != 0) {
        set_error("bad srtp key/salt hex"); goto out;
    }

    srtp_policy_t in_pol, out_pol;
    memset(&in_pol, 0, sizeof(in_pol));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in_pol.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in_pol.rtcp);
    in_pol.ssrc.type   = ssrc_specific;
    in_pol.ssrc.value  = g_refl_ssrc;
    in_pol.key         = master;
    in_pol.window_size = 1024;
    if (srtp_create(&g_srtp_in, &in_pol) != srtp_err_status_ok) {
        set_error("srtp_create(in) failed"); goto out;
    }
    memset(&out_pol, 0, sizeof(out_pol));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out_pol.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out_pol.rtcp);
    out_pol.ssrc.type     = ssrc_specific;
    out_pol.ssrc.value    = g_node_ssrc;
    out_pol.key           = master;
    out_pol.window_size   = 1024;
    out_pol.allow_repeat_tx = 1;
    if (srtp_create(&g_srtp_out, &out_pol) != srtp_err_status_ok) {
        srtp_dealloc(g_srtp_in);
        set_error("srtp_create(out) failed"); goto out;
    }
    g_session_active = 1;

    int err = 0;
    g_enc = opus_encoder_create(KERCHUNK_LINK_OPUS_SAMPLE_RATE, 1,
                                OPUS_APPLICATION_VOIP, &err);
    if (!g_enc) { set_error("opus_encoder_create %d", err); goto out; }
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(g_opus_bitrate));
    opus_encoder_ctl(g_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(g_enc, OPUS_SET_PACKET_LOSS_PERC(g_opus_loss_perc));

    g_dec = opus_decoder_create(KERCHUNK_LINK_OPUS_SAMPLE_RATE, 1, &err);
    if (!g_dec) { set_error("opus_decoder_create %d", err); goto out; }

    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) { set_error("socket: %s", strerror(errno)); goto out; }
    int flags = fcntl(g_udp_fd, F_GETFL, 0);
    fcntl(g_udp_fd, F_SETFL, flags | O_NONBLOCK);

    rc = 0;
out:
    free(ep); free(kh); free(sh);
    return rc;
}

/* ── Bootstrap (PT=99) so reflector learns our UDP source ─────────── */

static void send_bootstrap(void)
{
    if (!g_session_active || g_udp_fd < 0) return;
    uint8_t rtp[KERCHUNK_LINK_RTP_MAX_PACKET];
    rtp[0] = 0x80;
    rtp[1] = KERCHUNK_LINK_RTP_BOOTSTRAP_PT & 0x7f;
    uint16_t s = htons(g_out_seq++); memcpy(rtp + 2, &s, 2);
    uint32_t t = htonl(g_out_ts);    memcpy(rtp + 4, &t, 4);
    uint32_t ss = htonl(g_node_ssrc);memcpy(rtp + 8, &ss, 4);
    int len = 12;
    if (srtp_protect(g_srtp_out, rtp, &len) != srtp_err_status_ok) return;
    sendto(g_udp_fd, rtp, (size_t)len, 0,
           (struct sockaddr *)&g_rtp_dst, sizeof(g_rtp_dst));
}

/* ── Encode pipeline (drain ring → resample → Opus → SRTP → UDP) ──── */

#define LINK_FRAME_48K (KERCHUNK_LINK_OPUS_FRAME_MS * 48)  /* 60ms × 48 = 2880 */

static void encode_one_frame(const int16_t *pcm48k_2880)
{
    /* Downsample 48k → 24k by simple 2:1 decimation with a small box
     * filter (anti-alias). Works for voice because the source band
     * already rolls off long before 12 kHz. Replace with a proper IIR
     * if we ever feed studio-quality audio. */
    int16_t pcm24k[KERCHUNK_LINK_OPUS_FRAME_SAMPLES];
    for (int i = 0; i < KERCHUNK_LINK_OPUS_FRAME_SAMPLES; i++) {
        int32_t a = pcm48k_2880[2*i];
        int32_t b = pcm48k_2880[2*i + 1];
        pcm24k[i] = (int16_t)((a + b) / 2);
    }

    uint8_t opus_buf[400];
    int olen = opus_encode(g_enc, pcm24k,
                           KERCHUNK_LINK_OPUS_FRAME_SAMPLES,
                           opus_buf, sizeof(opus_buf));
    if (olen < 0) return;

    uint8_t rtp[KERCHUNK_LINK_RTP_MAX_PACKET];
    rtp[0] = 0x80;
    rtp[1] = KERCHUNK_LINK_RTP_PAYLOAD_TYPE & 0x7f;
    uint16_t s = htons(g_out_seq++); memcpy(rtp + 2, &s, 2);
    uint32_t t = htonl(g_out_ts);    memcpy(rtp + 4, &t, 4);
    g_out_ts += KERCHUNK_LINK_RTP_FRAME_TS_TICKS;
    uint32_t ss = htonl(g_node_ssrc);memcpy(rtp + 8, &ss, 4);
    memcpy(rtp + 12, opus_buf, (size_t)olen);
    int rtp_len = 12 + olen;

    if (srtp_protect(g_srtp_out, rtp, &rtp_len) != srtp_err_status_ok) return;
    if (sendto(g_udp_fd, rtp, (size_t)rtp_len, 0,
               (struct sockaddr *)&g_rtp_dst, sizeof(g_rtp_dst)) ==
        rtp_len) {
        g_pkts_sent++;
        g_bytes_sent += (uint64_t)rtp_len;
    }
}

static void drain_and_encode(void)
{
    if (!g_session_active || g_muted) {
        /* Disconnected OR muted: discard everything we accumulated so we
         * don't replay stale audio when service resumes. */
        __atomic_store_n(&g_ring_r, __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
        return;
    }
    int16_t buf[LINK_FRAME_48K];
    while (ring_avail_read() >= LINK_FRAME_48K) {
        size_t got = ring_read(buf, LINK_FRAME_48K);
        if (got == LINK_FRAME_48K) encode_one_frame(buf);
    }
}

/* Track loss for the quality report. Called every received audio packet
 * with the parsed RTP seq.
 *
 * The window is reset by send_quality_report() at the report tick —
 * not lazily on next-packet — so each report covers exactly one window
 * and a stale loss% can't be re-reported across silent periods.
 *
 * Out-of-order arrivals are credited back: when a packet shows up with
 * a seq lower than the highest we've seen, we previously counted it
 * as "missing" — decrement g_recv_lost. Without this, even a tiny
 * amount of LAN reordering pegs reported loss at a steady false rate
 * (15%+ with mild jitter), which would constantly trip the reflector. */
static void quality_track_seq(uint16_t seq)
{
    if (!g_recv_have_seq) {
        g_recv_have_seq = 1;
        g_recv_last_seq = seq;
        g_recv_received++;
        return;
    }
    int16_t delta = (int16_t)(seq - g_recv_last_seq);
    if (delta > 0) {
        /* Forward jump: delta-1 packets are presumed lost (still in
         * flight or genuinely dropped). Cap at 200 so a sender swap
         * doesn't attribute 60k packets of bogus loss. */
        if (delta < 200) g_recv_lost += (delta - 1);
        g_recv_last_seq = seq;
    } else if (delta < 0 && delta > -200) {
        /* Late arrival of something we'd already counted as lost. */
        if (g_recv_lost > 0) g_recv_lost--;
    }
    /* delta == 0 → duplicate. Don't double-count receipt either. */
    if (delta != 0) g_recv_received++;
}

static void send_quality_report(void)
{
    if (!g_ws || !g_session_active) return;
    /* Snapshot + clear so each report covers exactly the last window.
     * Without this, a brief loss burst followed by silence would re-
     * report the same loss% every 10s and trip the reflector's mute
     * over and over. */
    int lost     = g_recv_lost;
    int received = g_recv_received;
    g_recv_lost              = 0;
    g_recv_received          = 0;
    g_quality_window_start_ms = now_ms();

    int total        = received + lost;
    int loss_pct_x10 = total > 0 ? (lost * 1000 / total) : 0;

    /* Suppress reports with very few packets in the window — losing
     * 1 of 4 is 25%, statistically meaningless and would trip mute on
     * any brief reorder. 30 packets ≈ 1.8s of audio at 60ms frames,
     * enough to make the percentage trustworthy. */
    if (total < 30) return;

    int jb_depth_ms = 0;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"type\":\"%s\",\"loss_pct\":%d.%d,\"jitter_ms\":0,"
        "\"jb_depth_ms\":%d,\"decode_errs\":%d,\"window_s\":10}",
        LINK_MSG_QUALITY, loss_pct_x10 / 10, loss_pct_x10 % 10,
        jb_depth_ms, g_decode_err);
    send_ws_json(buf);
}

/* ── Receive pipeline (UDP → SRTP unprotect → Opus decode → queue) ── */

static void play_decoded_frame(const int16_t *pcm24k, int n)
{
    /* Upsample 24k → 48k by linear interpolation (cheap; voice is fine). */
    int16_t pcm48k[KERCHUNK_LINK_OPUS_FRAME_SAMPLES * 2];
    for (int i = 0; i < n; i++) {
        int16_t a = pcm24k[i];
        int16_t b = (i + 1 < n) ? pcm24k[i + 1] : pcm24k[i];
        pcm48k[2*i]     = a;
        pcm48k[2*i + 1] = (int16_t)(((int32_t)a + b) / 2);
    }
    int dst_rate = g_core->sample_rate;
    if (dst_rate != 48000) {
        /* Re-resample if core isn't running at 48k. Reuses the
         * project-wide kerchunk_resample helper (kerchunk_queue_add_buffer_src
         * has its own variant — but easier to just convert first). */
        int16_t out[2048];
        size_t out_n = kerchunk_resample_into(out, sizeof(out)/sizeof(out[0]),
                                              pcm48k, (size_t)(n * 2),
                                              48000, dst_rate);
        if (out_n > 0)
            kerchunk_queue_add_buffer_src(out, out_n,
                                          KERCHUNK_PRI_ELEVATED, 0, "link");
    } else {
        kerchunk_queue_add_buffer_src(pcm48k, (size_t)(n * 2),
                                      KERCHUNK_PRI_ELEVATED, 0, "link");
    }

    /* Auto-PTT while audio is flowing; release after link_tail_ms quiet. */
    if (!g_link_ptt_held) {
        g_core->request_ptt("link");
        g_link_ptt_held = 1;
    }
    g_link_last_recv_ms = now_ms();
}

static void drain_udp_recv(void)
{
    if (g_udp_fd < 0 || !g_session_active) return;
    uint8_t pkt[KERCHUNK_LINK_RTP_MAX_PACKET];
    int16_t pcm[KERCHUNK_LINK_OPUS_FRAME_SAMPLES];
    while (1) {
        ssize_t n = recv(g_udp_fd, pkt, sizeof(pkt), 0);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                            "udp recv: %s", strerror(errno));
            break;
        }
        g_pkts_recv++;
        g_bytes_recv += (uint64_t)n;
        int len = (int)n;
        if (srtp_unprotect(g_srtp_in, pkt, &len) != srtp_err_status_ok) {
            g_srtp_auth_fail++;
            int64_t nowm = now_ms();
            if (nowm - g_srtp_window_start_ms > SRTP_DRIFT_WINDOW_MS) {
                g_srtp_window_start_ms = nowm;
                g_srtp_window_count    = 0;
            }
            if (++g_srtp_window_count > SRTP_DRIFT_THRESHOLD) {
                set_error("SRTP auth fail > %d/10s — reconnecting (key drift)",
                          SRTP_DRIFT_THRESHOLD);
                g_srtp_window_count    = 0;
                g_srtp_window_start_ms = nowm;
                if (g_ws) g_ws->is_closing = 1;
                return;
            }
            continue;
        }
        if (len <= 12) continue;
        uint16_t seq;
        memcpy(&seq, pkt + 2, 2);
        seq = ntohs(seq);
        quality_track_seq(seq);
        int dlen = opus_decode(g_dec, pkt + 12, len - 12,
                               pcm, KERCHUNK_LINK_OPUS_FRAME_SAMPLES, 0);
        if (dlen <= 0) { g_decode_err++; continue; }
        play_decoded_frame(pcm, dlen);
    }

    /* PTT tail-down. */
    if (g_link_ptt_held &&
        now_ms() - g_link_last_recv_ms > g_link_tail_ms) {
        g_core->release_ptt("link");
        g_link_ptt_held = 0;
    }
}

/* ── WS protocol handlers ─────────────────────────────────────────── */

static void send_ws_json(const char *json)
{
    if (g_ws) mg_ws_send(g_ws, json, strlen(json), WEBSOCKET_OP_TEXT);
}

static void on_hello(struct mg_str body)
{
    char *chal_hex = mg_json_get_str(body, "$.challenge");
    if (!chal_hex || strlen(chal_hex) != 2 * KERCHUNK_LINK_CHALLENGE_BYTES) {
        set_error("hello: bad challenge"); free(chal_hex);
        { if (g_ws) g_ws->is_closing = 1; return; }
    }
    uint8_t challenge[KERCHUNK_LINK_CHALLENGE_BYTES];
    if (hex_to_bytes(chal_hex, challenge, sizeof(challenge)) != 0) {
        set_error("hello: challenge not hex"); free(chal_hex);
        { if (g_ws) g_ws->is_closing = 1; return; }
    }
    free(chal_hex);

    uint8_t nonce[KERCHUNK_LINK_NONCE_BYTES];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
        set_error("RAND_bytes"); { if (g_ws) g_ws->is_closing = 1; return; }
    }
    uint8_t input[KERCHUNK_LINK_CHALLENGE_BYTES + KERCHUNK_LINK_NONCE_BYTES];
    memcpy(input,                                challenge, sizeof(challenge));
    memcpy(input + KERCHUNK_LINK_CHALLENGE_BYTES, nonce,    sizeof(nonce));
    uint8_t  mac[KERCHUNK_LINK_HMAC_BYTES]; unsigned mac_len = sizeof(mac);
    if (!HMAC(EVP_sha256(), g_psk, sizeof(g_psk),
              input, sizeof(input), mac, &mac_len)) {
        set_error("HMAC"); { if (g_ws) g_ws->is_closing = 1; return; }
    }
    char nh[KERCHUNK_LINK_NONCE_BYTES * 2 + 1];
    char mh[KERCHUNK_LINK_HMAC_BYTES   * 2 + 1];
    bytes_to_hex(nonce, sizeof(nonce), nh);
    bytes_to_hex(mac,   sizeof(mac),   mh);

    char buf[KERCHUNK_LINK_MAX_MSG];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"%s\",\"node_id\":\"%s\","
             "\"key_hmac\":\"%s\",\"nonce\":\"%s\","
             "\"client_version\":\"kerchunk %s\"}",
             LINK_MSG_LOGIN, g_node_id, mh, nh, "1.0");
    send_ws_json(buf);
    set_state(LST_AWAIT_LOGIN_OK);
}

static void on_login_ok(struct mg_str body)
{
    if (session_setup(body) != 0) {
        { if (g_ws) g_ws->is_closing = 1; return; }
    }
    set_state(LST_CONNECTED);
    g_reconnect_attempt = 0;
    g_last_error[0] = '\0';
    send_bootstrap();
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "connected to %s as %s on TG %d",
                g_reflector_ws, g_node_id, g_current_tg);
    publish_snapshot();
}

static void on_login_denied(struct mg_str body)
{
    char *code = mg_json_get_str(body, "$.code");
    char *msg  = mg_json_get_str(body, "$.msg");
    set_error("login_denied(%s): %s", code ? code : "?", msg ? msg : "");
    if (link_err_is_permanent(code)) {
        set_state(LST_STOPPED);
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "permanent login error '%s' — operator must clear",
                    code);
    }
    free(code); free(msg);
    if (g_ws) g_ws->is_closing = 1;
}

static void on_talker(struct mg_str body)
{
    char *who = mg_json_get_str(body, "$.node_id");
    snprintf(g_current_talker, sizeof(g_current_talker), "%s",
             who ? who : "");
    free(who);
    publish_snapshot();
}

/* Phase 7 — protocol-defined deny / revoke / shutdown / mute paths.
 * Each surfaces in g_last_error so the operator can see what happened. */

static void on_tg_ok(struct mg_str body)
{
    long tg = mg_json_get_long(body, "$.tg", 0);
    if (tg <= 0 || tg > 65535) return;
    g_current_tg = (int)tg;
    set_error("");
    publish_snapshot();
}

static void on_tg_denied(struct mg_str body)
{
    char *code = mg_json_get_str(body, "$.code");
    long  tg   = mg_json_get_long(body, "$.tg", 0);
    set_error("tg_denied(tg=%ld, %s)", tg, code ? code : "?");
    free(code);
}

static void on_floor_denied(struct mg_str body)
{
    char *cur = mg_json_get_str(body, "$.current_talker");
    /* Don't store as a sticky error — it's a transient signal — but do
     * surface it on the snapshot so the dashboard can flash a "TG busy"
     * indicator. */
    set_error("TG busy (current talker: %s)", cur ? cur : "?");
    free(cur);
}

static void on_floor_revoked(struct mg_str body)
{
    char *code = mg_json_get_str(body, "$.code");
    if (code && strcmp(code, LINK_ERR_LEASE_EXPIRED) != 0) {
        /* lease_expired is normal end-of-transmission — not worth
         * showing. admin / auth_failures are. */
        set_error("floor_revoked(%s)", code);
    }
    free(code);
}

static void on_reflector_shutdown(struct mg_str body)
{
    long restart_in_s = mg_json_get_long(body, "$.restart_in_s", 15);
    char *reason = mg_json_get_str(body, "$.reason");
    set_error("reflector_shutdown(%s) — retry in %lds",
              reason ? reason : "?", restart_in_s);
    free(reason);
    /* Override the next-reconnect time to match the server's hint, with
     * ±20% jitter so 100 nodes don't reconnect in lockstep. */
    int64_t base = restart_in_s * 1000;
    int     jit  = (int)((rand() % (base / 5 + 1)) - base / 10);
    g_next_reconnect_ms = now_ms() + base + jit;
    /* Increment attempt so the regular backoff (if reconnect later
     * still fails) starts conservative. */
    g_reconnect_attempt = 1;
    if (g_ws) g_ws->is_closing = 1;
}

static void on_mute(struct mg_str body)
{
    char *reason = mg_json_get_str(body, "$.reason");
    set_error("muted by reflector (%s)", reason ? reason : "?");
    g_muted = 1;
    free(reason);
}

static void on_unmute(void)
{
    set_error("");
    g_muted = 0;
}

static void on_target_bitrate(struct mg_str body)
{
    long bps = mg_json_get_long(body, "$.bps", 0);
    if (bps < 8000 || bps > 64000) return;   /* sane bounds */
    if (bps == g_current_bitrate) return;
    if (g_enc) opus_encoder_ctl(g_enc, OPUS_SET_BITRATE((int)bps));
    g_current_bitrate = (int)bps;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "target_bitrate from reflector: %ld bps", bps);
}

/* Reflector pushes a fresh roster whenever a peer joins/leaves this TG
 * (login, disconnect, set_tg). We re-stash the JSON array so the next
 * SSE snapshot reflects current online/offline state in real time. */
static void on_tg_roster(struct mg_str body)
{
    struct mg_str members = mg_json_get_tok(body, "$.members");
    if (!members.buf || members.len == 0 ||
        members.len >= sizeof(g_tg_members_json)) return;
    memcpy(g_tg_members_json, members.buf, members.len);
    g_tg_members_json[members.len] = '\0';
    publish_snapshot();   /* push to dashboard immediately */
}

static void on_tg_membership_changed(struct mg_str body)
{
    long old_tg = mg_json_get_long(body, "$.old_tg", 0);
    long new_tg = mg_json_get_long(body, "$.new_tg", 0);
    char *reason = mg_json_get_str(body, "$.reason");
    set_error("TG changed by reflector: %ld → %ld (%s)",
              old_tg, new_tg, reason ? reason : "?");
    g_current_tg = (int)new_tg;
    free(reason);
}

static void on_kicked(struct mg_str body)
{
    char *code = mg_json_get_str(body, "$.code");
    char *msg  = mg_json_get_str(body, "$.msg");
    set_error("kicked: %s%s%s",
              code ? code : "?",
              msg && msg[0] ? " — " : "",
              msg && msg[0] ? msg   : "");
    if (link_err_is_permanent(code)) {
        set_state(LST_STOPPED);
    } else {
        /* Transient kick (admin_action, idle_timeout, loss_too_high,
         * auth_failures, etc.) — back off 30 s ±20 % so the operator
         * has a window to investigate before we hammer back. Without
         * this we'd reconnect in reconnect_min_ms (default 1 s) which
         * makes the kick effectively a no-op. */
        int base = 30000;
        int jit  = (rand() % (base / 5 + 1)) - base / 10;
        g_next_reconnect_ms = now_ms() + base + jit;
        g_reconnect_attempt = 1;
    }
    free(code); free(msg);
    if (g_ws) g_ws->is_closing = 1;
    publish_snapshot();
}

static void on_ws_msg(struct mg_str body)
{
    char *type = mg_json_get_str(body, "$.type");
    if (!type) return;

    if      (!strcmp(type, LINK_MSG_HELLO))             on_hello(body);
    else if (!strcmp(type, LINK_MSG_LOGIN_OK))          on_login_ok(body);
    else if (!strcmp(type, LINK_MSG_LOGIN_DENIED))      on_login_denied(body);
    else if (!strcmp(type, LINK_MSG_TALKER))            on_talker(body);
    else if (!strcmp(type, LINK_MSG_TG_OK))             on_tg_ok(body);
    else if (!strcmp(type, LINK_MSG_TG_DENIED))         on_tg_denied(body);
    else if (!strcmp(type, LINK_MSG_FLOOR_DENIED))      on_floor_denied(body);
    else if (!strcmp(type, LINK_MSG_FLOOR_REVOKED))     on_floor_revoked(body);
    else if (!strcmp(type, LINK_MSG_REFLECTOR_SHUTDOWN))on_reflector_shutdown(body);
    else if (!strcmp(type, LINK_MSG_MUTE))              on_mute(body);
    else if (!strcmp(type, LINK_MSG_UNMUTE))            on_unmute();
    else if (!strcmp(type, LINK_MSG_TARGET_BITRATE))    on_target_bitrate(body);
    else if (!strcmp(type, LINK_MSG_TG_ROSTER))         on_tg_roster(body);
    else if (!strcmp(type, LINK_MSG_TG_MEMBERSHIP_CHANGED)) on_tg_membership_changed(body);
    else if (!strcmp(type, LINK_MSG_KICKED))            on_kicked(body);
    else if (!strcmp(type, LINK_MSG_PONG)) {
        /* Just receiving the pong is enough — refreshes our peer-alive
         * timer (which is per-conn and managed by the server). We don't
         * track our own pong window in the client right now because the
         * mongoose poll already fires MG_EV_CLOSE on TCP timeout. */
    }
    else if (!strcmp(type, LINK_MSG_PING)) {
        long seq = mg_json_get_long(body, "$.seq", 0);
        char b[64];
        snprintf(b, sizeof(b), "{\"type\":\"%s\",\"seq\":%ld}",
                 LINK_MSG_PONG, seq);
        send_ws_json(b);
    }
    free(type);
    publish_snapshot();
}

/* ── Mongoose event handler ───────────────────────────────────────── */

static void evh(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_OPEN) {
        c->is_hexdumping = 0;
    } else if (ev == MG_EV_CONNECT && c->is_tls) {
        /* Mongoose marks the conn as TLS from the wss:// URL but does not
         * init TLS itself — we have to do it here. */
        struct mg_str host = mg_url_host(g_reflector_ws);
        struct mg_tls_opts opts = {
            .name              = host,
            .skip_verification = g_verify_peer ? 0 : 1,
        };
        mg_tls_init(c, &opts);
    } else if (ev == MG_EV_WS_OPEN) {
        set_state(LST_AWAIT_HELLO);
    } else if (ev == MG_EV_WS_MSG) {
        struct mg_ws_message *wm = (struct mg_ws_message *)ev_data;
        on_ws_msg(wm->data);
    } else if (ev == MG_EV_ERROR) {
        set_error("ws: %s", ev_data ? (char *)ev_data : "(unknown)");
    } else if (ev == MG_EV_CLOSE) {
        if (c == g_ws) {
            g_ws = NULL;
            session_cleanup();
            if (g_state != LST_STOPPED) {
                set_state(LST_RECONNECTING);
                /* If reflector_shutdown (or any other path) already scheduled
                 * a deferred reconnect, honor that — don't stomp it with the
                 * default min_ms. */
                int64_t now = now_ms();
                if (g_next_reconnect_ms <= now) {
                    int delay = g_reconnect_min_ms;
                    for (int i = 1; i < g_reconnect_attempt && delay < g_reconnect_max_ms; i++)
                        delay *= 2;
                    if (delay > g_reconnect_max_ms) delay = g_reconnect_max_ms;
                    int jit = (rand() % (delay / 5 + 1)) - delay / 10;
                    g_next_reconnect_ms = now + delay + jit;
                    g_reconnect_attempt++;
                }
            }
        }
    }
}

/* ── Background thread ────────────────────────────────────────────── */

static void *bg_main(void *ud)
{
    (void)ud;
    int64_t next_send_ms     = 0;
    int64_t next_snapshot_ms = 0;
    int64_t next_quality_ms  = 0;

    while (g_run) {
        mg_mgr_poll(&g_mgr, 10);

        int64_t now = now_ms();

        /* Pending CLI / DTMF requests. */
        if (g_req_clear_alarm) {
            g_req_clear_alarm = 0;
            if (g_state == LST_STOPPED) {
                g_last_error[0] = '\0';
                g_reconnect_attempt = 0;
                g_next_reconnect_ms = 0;
                set_state(LST_RECONNECTING);
            }
        }
        if (g_req_reconnect && g_ws) {
            g_req_reconnect = 0;
            g_ws->is_closing = 1;
        } else if (g_req_reconnect) {
            g_req_reconnect = 0;
            g_next_reconnect_ms = 0;   /* trigger immediate reconnect */
        }
        if (g_req_set_tg && g_ws && g_state == LST_CONNECTED) {
            int tg = g_req_set_tg;
            g_req_set_tg = 0;
            char buf[64];
            snprintf(buf, sizeof(buf),
                     "{\"type\":\"%s\",\"tg\":%d}", LINK_MSG_SET_TG, tg);
            send_ws_json(buf);
        }

        /* Periodic snapshot — keeps live counters fresh on the SSE bus
         * even when the state machine is quiet. 1 s matches reflectd's
         * dashboard cadence; the publish is just a snprintf + sse_publish
         * (single string copy into per-client ringbuffers), well under
         * a millisecond on the Pi. */
        if (now >= next_snapshot_ms) {
            publish_snapshot();
            next_snapshot_ms = now + 1000;
        }
        /* LINK-PROTOCOL.md § 4.1.4: send a quality report every 10s. */
        if (now >= next_quality_ms && g_state == LST_CONNECTED) {
            send_quality_report();
            next_quality_ms = now + QUALITY_REPORT_MS;
        }

        /* Connect / reconnect logic. Gated on g_enabled so the thread
         * sits idle until configure() supplies a usable [link] section
         * (and stays idle when [link] enabled = off). */
        if (g_enabled && !g_ws && g_state != LST_STOPPED) {
            if (now >= g_next_reconnect_ms) {
                set_state(LST_CONNECTING);
                g_ws = mg_ws_connect(&g_mgr, g_reflector_ws, evh, NULL, NULL);
                if (!g_ws) {
                    set_error("mg_ws_connect to %s failed", g_reflector_ws);
                    g_next_reconnect_ms = now + g_reconnect_min_ms;
                }
            }
        }

        /* Drain UDP audio + PTT tail. */
        drain_udp_recv();

        /* Pace local-RX encode at 60 ms cadence. */
        if (g_session_active && now >= next_send_ms) {
            drain_and_encode();
            next_send_ms = now + KERCHUNK_LINK_OPUS_FRAME_MS;
        }

        /* NAT keepalive: PT=99 marker every NAT_KEEPALIVE_MS when no
         * audio has gone out recently. Cheap (12-byte SRTP packet) and
         * keeps the upstream mapping alive across long quiet periods. */
        if (g_session_active && now - g_last_nat_keepalive_ms >= NAT_KEEPALIVE_MS) {
            send_bootstrap();
            g_last_nat_keepalive_ms = now;
        }
    }
    return NULL;
}

/* ── Audio tap (audio thread) ─────────────────────────────────────── */

static void link_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_local_rx_active || !evt->audio.samples || evt->audio.n == 0) return;
    if (g_core->sample_rate != 48000) {
        /* mod_link's downsampler is hard-coded for 48→24. Fall back to
         * not encoding rather than producing wrong audio. */
        return;
    }
    if (ring_avail_write() < evt->audio.n) return;   /* silently drop on overflow */
    ring_write(evt->audio.samples, evt->audio.n);
}

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{ (void)evt; (void)ud; g_local_rx_active = 1; }

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{ (void)evt; (void)ud; g_local_rx_active = 0; }

/* ── CLI commands ─────────────────────────────────────────────────── */

static int cli_link_status(kerchunk_resp_t *r)
{
    resp_str(r, "state",          state_name(g_state));
    resp_str(r, "node_id",        g_node_id);
    resp_str(r, "reflector_ws",   g_reflector_ws);
    resp_int(r, "tg",             g_current_tg);
    resp_str(r, "current_talker", g_current_talker[0] ? g_current_talker : "");
    resp_int(r, "reconnect_attempt", g_reconnect_attempt);
    resp_str(r, "last_error",     g_last_error);
    long uptime_s = g_session_started_at
        ? (long)((int64_t)time(NULL) - g_session_started_at) : 0;
    resp_int(r, "session_started_at", (int)g_session_started_at);
    resp_int(r, "uptime_s",       (int)uptime_s);
    resp_int(r, "pkts_sent",      g_pkts_sent);
    resp_int(r, "pkts_recv",      g_pkts_recv);
    resp_int(r, "bytes_sent",     (int)g_bytes_sent);
    resp_int(r, "bytes_recv",     (int)g_bytes_recv);
    resp_int(r, "srtp_auth_fail", g_srtp_auth_fail);
    resp_int(r, "decode_err",     g_decode_err);
    return 0;
}

static int cli_link(int argc, const char **argv, kerchunk_resp_t *r)
{
    /* `link` and `link status` print the snapshot. */
    if (argc < 2 || !strcmp(argv[1], "status"))
        return cli_link_status(r);

    if (!strcmp(argv[1], "tg")) {
        if (argc < 3) {
            resp_str(r, "error", "usage: link tg <number>");
            return -1;
        }
        int tg = atoi(argv[2]);
        if (tg <= 0 || tg > 65535) {
            resp_str(r, "error", "tg must be 1..65535");
            return -1;
        }
        g_req_set_tg = tg;       /* bg thread picks this up */
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "set_tg requested");
        resp_int(r, "tg", tg);
        return 0;
    }

    if (!strcmp(argv[1], "reconnect")) {
        g_req_reconnect = 1;
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "reconnect requested");
        return 0;
    }

    if (!strcmp(argv[1], "clear-alarm")) {
        g_req_clear_alarm = 1;
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "clear-alarm requested");
        return 0;
    }

    resp_str(r, "error", "unknown subcommand (try: status, tg <n>, reconnect, clear-alarm)");
    return -1;
}

/* ── DTMF *73<n># handler ─────────────────────────────────────────── */

#define LINK_DTMF_EVT_OFFSET 73   /* arbitrary slot in KERCHEVT_CUSTOM space */

static void on_dtmf_set_tg(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    /* arg is the digits AFTER "73" — e.g. "1" for *731# → TG 1. */
    const char *arg = (const char *)evt->custom.data;
    if (!arg || !arg[0]) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "DTMF *73# with no TG number — ignoring");
        return;
    }
    int tg = atoi(arg);
    if (tg <= 0 || tg > 65535) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "DTMF *73<%s># — invalid TG", arg);
        return;
    }
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "DTMF *73<%d># → request TG switch", tg);
    g_req_set_tg = tg;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void apply_defaults(void)
{
    g_default_tg       = 0;
    g_link_tail_ms     = 500;
    g_jb_target_ms     = 100;
    g_opus_bitrate     = KERCHUNK_LINK_OPUS_BITRATE;
    g_opus_loss_perc   = 10;
    g_reconnect_min_ms = 1000;
    g_reconnect_max_ms = 60000;
}

static int configure_(const kerchunk_config_t *cfg)
{
    apply_defaults();
    /* Match the project convention: enabled = on / off (other strings
     * accepted as on for tolerance, but the documented form is `on`). */
    const char *en = kerchunk_config_get(cfg, "link", "enabled");
    g_enabled = en && (!strcmp(en, "on")  || !strcmp(en, "true") ||
                       !strcmp(en, "yes") || !strcmp(en, "1"));
    if (!g_enabled) {
        set_state(LST_DISABLED);
        return 0;
    }

    const char *node_id = kerchunk_config_get(cfg, "link", "node_id");
    const char *url     = kerchunk_config_get(cfg, "link", "reflector_ws");
    const char *psk_hex = kerchunk_config_get(cfg, "link", "preshared_key_hex");
    if (!node_id || !url || !psk_hex) {
        set_error("[link] requires node_id, reflector_ws, preshared_key_hex");
        g_enabled = 0; set_state(LST_STOPPED);
        return -1;
    }
    snprintf(g_node_id,       sizeof(g_node_id),       "%s", node_id);
    snprintf(g_reflector_ws,  sizeof(g_reflector_ws),  "%s", url);
    if (strlen(psk_hex) != 2 * KERCHUNK_LINK_PSK_BYTES ||
        hex_to_bytes(psk_hex, g_psk, KERCHUNK_LINK_PSK_BYTES) != 0) {
        set_error("[link] preshared_key_hex must be %d hex chars",
                  KERCHUNK_LINK_PSK_BYTES * 2);
        g_enabled = 0; set_state(LST_STOPPED);
        return -1;
    }
    g_psk_loaded = 1;

    g_default_tg     = kerchunk_config_get_int(cfg, "link", "default_tg",     0);
    g_link_tail_ms   = kerchunk_config_get_int(cfg, "link", "link_tail_ms",   500);
    g_jb_target_ms   = kerchunk_config_get_int(cfg, "link", "jitter_target_ms", 100);
    g_opus_bitrate   = kerchunk_config_get_int(cfg, "link", "opus_bitrate",   KERCHUNK_LINK_OPUS_BITRATE);
    g_opus_loss_perc = kerchunk_config_get_int(cfg, "link", "opus_loss_perc", 10);
    g_reconnect_min_ms = kerchunk_config_get_int(cfg, "link", "reconnect_min_ms", 1000);
    g_reconnect_max_ms = kerchunk_config_get_int(cfg, "link", "reconnect_max_ms", 60000);
    {
        /* Default ON now that mongoose serves the full chain (vendor
         * patch in mg_tls_init that walks the PEM after the leaf and
         * adds intermediates via SSL_add1_chain_cert). Operators can
         * still set verify_peer = off for self-signed lab certs. */
        const char *vp = kerchunk_config_get(cfg, "link", "verify_peer");
        g_verify_peer = (vp == NULL) ? 1
            : (!strcmp(vp, "on") || !strcmp(vp, "true") ||
               !strcmp(vp, "yes") || !strcmp(vp, "1")) ? 1 : 0;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "configured: node=%s reflector=%s tg=%d bitrate=%d verify_peer=%d",
                g_node_id, g_reflector_ws, g_default_tg, g_opus_bitrate, g_verify_peer);
    return 0;
}

static int load_(kerchunk_core_t *core)
{
    g_core = core;
    apply_defaults();

    /* libsrtp init is process-wide; safe to call multiple times — guard
     * against double-init in case mod_web also pulls it in someday. */
    static int g_srtp_inited;
    if (!g_srtp_inited) {
        if (srtp_init() != srtp_err_status_ok) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "srtp_init failed");
            return -1;
        }
        g_srtp_inited = 1;
    }

    mg_log_set(MG_LL_ERROR);
    mg_mgr_init(&g_mgr);

    core->subscribe(KERCHEVT_COR_ASSERT, on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,   on_cor_drop,   NULL);
    core->subscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + LINK_DTMF_EVT_OFFSET),
                    on_dtmf_set_tg, NULL);
    core->audio_tap_register(link_audio_tap, NULL);

    /* Register *73<n># for runtime TG switch (mod_dtmfcmd may load
     * after us — registration is idempotent and harmless if vtable
     * isn't wired yet, but in practice load order has dtmfcmd first). */
    if (core->dtmf_register) {
        core->dtmf_register("73", LINK_DTMF_EVT_OFFSET,
                            "Link: switch talkgroup",
                            "link_set_tg_pattern");
    }

    g_run = 1;
    if (pthread_create(&g_thread, NULL, bg_main, NULL) != 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "pthread_create failed");
        return -1;
    }
    g_thread_started = 1;

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "loaded");
    return 0;
}

static void unload_(void)
{
    g_run = 0;
    if (g_thread_started) {
        pthread_join(g_thread, NULL);
        g_thread_started = 0;
    }
    if (g_core) {
        g_core->audio_tap_unregister(link_audio_tap);
        g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_assert);
        g_core->unsubscribe(KERCHEVT_COR_DROP,   on_cor_drop);
        g_core->unsubscribe(
            (kerchevt_type_t)(KERCHEVT_CUSTOM + LINK_DTMF_EVT_OFFSET),
            on_dtmf_set_tg);
        if (g_core->dtmf_unregister) g_core->dtmf_unregister("73");
    }
    session_cleanup();
    mg_mgr_free(&g_mgr);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded");
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name        = "link",
      .usage       = "link [status|tg <n>|reconnect|clear-alarm]",
      .description = "Repeater-link status / control",
      .handler     = cli_link,
      .category    = "Audio",
      .subcommands = "status,tg,reconnect,clear-alarm" },
};

static kerchunk_module_def_t mod_link = {
    .name             = "mod_link",
    .version          = "0.1.0",
    .description      = "Bridge to a kerchunk-reflectd network",
    .load             = load_,
    .configure        = configure_,
    .unload           = unload_,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_link);
