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

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <opus/opus.h>
#include <srtp2/srtp.h>
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

    /* Phase 3: audio modes */
    int           audio_send_ms;      /* >0: synthesize + send this much audio */
    int           audio_recv_ms;      /* >0: receive for this long, then exit */
    char          rtp_endpoint[80];   /* "host:port" from login_ok */
    uint8_t       master_key[KERCHUNK_LINK_SRTP_KEY_BYTES];
    uint8_t       master_salt[KERCHUNK_LINK_SRTP_SALT_BYTES];
    uint32_t      node_ssrc;
    uint32_t      refl_ssrc;
    int           udp_fd;
    srtp_t        srtp_in;
    srtp_t        srtp_out;
    OpusEncoder  *opus_enc;
    OpusDecoder  *opus_dec;
    int64_t       audio_started_ms;
    int           pkts_sent;
    int           pkts_recv_total;
    int           pkts_recv_authed;
    int           frames_decoded;
    int           frames_plc;
    int           frames_late;
    uint16_t      out_seq;
    uint32_t      out_ts;

    /* Receive-side jitter buffer (LINK-PROTOCOL.md § 4.4) */
    int           jb_initialized;
    uint16_t      jb_next_seq;
    int64_t       jb_next_play_ms;

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

/* ── Phase 3 audio plane ──────────────────────────────────────────── */

static int parse_endpoint(const char *s, char *host, size_t hlen, int *port)
{
    const char *colon = strrchr(s, ':');
    if (!colon) return -1;
    size_t hl = (size_t)(colon - s);
    if (hl >= hlen) return -1;
    memcpy(host, s, hl);
    host[hl] = '\0';
    *port = atoi(colon + 1);
    return *port > 0 ? 0 : -1;
}

static int audio_init(void)
{
    if (srtp_init() != srtp_err_status_ok) {
        fail(NULL, "srtp_init failed"); return -1;
    }

    /* Combined master key||salt for libsrtp policy. */
    static uint8_t master[KERCHUNK_LINK_SRTP_KEY_BYTES +
                          KERCHUNK_LINK_SRTP_SALT_BYTES];
    memcpy(master,                              g.master_key,
           KERCHUNK_LINK_SRTP_KEY_BYTES);
    memcpy(master + KERCHUNK_LINK_SRTP_KEY_BYTES, g.master_salt,
           KERCHUNK_LINK_SRTP_SALT_BYTES);

    /* IN: decrypts packets from reflector. SSRC = refl_ssrc. */
    srtp_policy_t in_pol;
    memset(&in_pol, 0, sizeof(in_pol));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in_pol.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&in_pol.rtcp);
    in_pol.ssrc.type    = ssrc_specific;
    in_pol.ssrc.value   = g.refl_ssrc;
    in_pol.key          = master;
    in_pol.window_size  = 1024;
    if (srtp_create(&g.srtp_in, &in_pol) != srtp_err_status_ok) {
        fail(NULL, "srtp_create(in) failed"); return -1;
    }

    /* OUT: encrypts packets to reflector. SSRC = node_ssrc. */
    srtp_policy_t out_pol;
    memset(&out_pol, 0, sizeof(out_pol));
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out_pol.rtp);
    srtp_crypto_policy_set_aes_cm_128_hmac_sha1_80(&out_pol.rtcp);
    out_pol.ssrc.type     = ssrc_specific;
    out_pol.ssrc.value    = g.node_ssrc;
    out_pol.key           = master;
    out_pol.window_size   = 1024;
    out_pol.allow_repeat_tx = 1;
    if (srtp_create(&g.srtp_out, &out_pol) != srtp_err_status_ok) {
        fail(NULL, "srtp_create(out) failed"); return -1;
    }

    int err = 0;
    g.opus_enc = opus_encoder_create(KERCHUNK_LINK_OPUS_SAMPLE_RATE, 1,
                                     OPUS_APPLICATION_VOIP, &err);
    if (!g.opus_enc) { fail(NULL, "opus_encoder_create %d", err); return -1; }
    opus_encoder_ctl(g.opus_enc, OPUS_SET_BITRATE(KERCHUNK_LINK_OPUS_BITRATE));
    opus_encoder_ctl(g.opus_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(g.opus_enc, OPUS_SET_PACKET_LOSS_PERC(10));

    g.opus_dec = opus_decoder_create(KERCHUNK_LINK_OPUS_SAMPLE_RATE, 1, &err);
    if (!g.opus_dec) { fail(NULL, "opus_decoder_create %d", err); return -1; }

    /* Bind ephemeral UDP socket. */
    g.udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g.udp_fd < 0) { fail(NULL, "socket: %s", strerror(errno)); return -1; }
    struct sockaddr_in any;
    memset(&any, 0, sizeof(any));
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g.udp_fd, (struct sockaddr *)&any, sizeof(any)) != 0) {
        fail(NULL, "bind: %s", strerror(errno)); return -1;
    }
    int flags = fcntl(g.udp_fd, F_GETFL, 0);
    fcntl(g.udp_fd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

/* Send an empty (header-only) RTP packet with PT=99 to register our UDP
 * source addr at the reflector without engaging the floor — see
 * KERCHUNK_LINK_RTP_BOOTSTRAP_PT. */
static int audio_send_bootstrap(void)
{
    uint8_t rtp[KERCHUNK_LINK_RTP_MAX_PACKET];
    rtp[0] = 0x80;
    rtp[1] = KERCHUNK_LINK_RTP_BOOTSTRAP_PT & 0x7f;
    uint16_t seq_n = htons(g.out_seq++);
    memcpy(rtp + 2, &seq_n, 2);
    uint32_t ts_n = htonl(g.out_ts);
    memcpy(rtp + 4, &ts_n, 4);
    uint32_t ssrc_n = htonl(g.node_ssrc);
    memcpy(rtp + 8, &ssrc_n, 4);
    int rtp_len = 12;

    if (srtp_protect(g.srtp_out, rtp, &rtp_len) != srtp_err_status_ok)
        return -1;

    char host[64]; int port = 0;
    if (parse_endpoint(g.rtp_endpoint, host, sizeof(host), &port) != 0)
        return -1;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) return -1;

    return sendto(g.udp_fd, rtp, (size_t)rtp_len, 0,
                  (struct sockaddr *)&dst, sizeof(dst)) == rtp_len ? 0 : -1;
}

static int audio_send_one_frame(void)
{
    /* Generate one 60ms frame of 440Hz sine (24 kHz mono int16). */
    int16_t pcm[KERCHUNK_LINK_OPUS_FRAME_SAMPLES];
    static int phase = 0;
    for (int i = 0; i < KERCHUNK_LINK_OPUS_FRAME_SAMPLES; i++) {
        double t = (double)(phase + i) / KERCHUNK_LINK_OPUS_SAMPLE_RATE;
        pcm[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * 440.0 * t));
    }
    phase += KERCHUNK_LINK_OPUS_FRAME_SAMPLES;

    uint8_t opus_buf[400];
    int olen = opus_encode(g.opus_enc, pcm,
                           KERCHUNK_LINK_OPUS_FRAME_SAMPLES,
                           opus_buf, sizeof(opus_buf));
    if (olen < 0) return -1;

    /* Build RTP header. */
    uint8_t rtp[KERCHUNK_LINK_RTP_MAX_PACKET];
    rtp[0] = 0x80;                                /* V=2 */
    rtp[1] = KERCHUNK_LINK_RTP_PAYLOAD_TYPE & 0x7f;
    uint16_t seq_n = htons(g.out_seq++);
    memcpy(rtp + 2, &seq_n, 2);
    uint32_t ts_n = htonl(g.out_ts);
    g.out_ts += KERCHUNK_LINK_RTP_FRAME_TS_TICKS;
    memcpy(rtp + 4, &ts_n, 4);
    uint32_t ssrc_n = htonl(g.node_ssrc);
    memcpy(rtp + 8, &ssrc_n, 4);
    memcpy(rtp + 12, opus_buf, (size_t)olen);
    int rtp_len = 12 + olen;

    if (srtp_protect(g.srtp_out, rtp, &rtp_len) != srtp_err_status_ok)
        return -1;

    char host[64]; int port = 0;
    if (parse_endpoint(g.rtp_endpoint, host, sizeof(host), &port) != 0)
        return -1;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) return -1;

    if (sendto(g.udp_fd, rtp, (size_t)rtp_len, 0,
               (struct sockaddr *)&dst, sizeof(dst)) != rtp_len) return -1;
    g.pkts_sent++;
    return 0;
}

/* Jitter buffer. Fixed depth covers half a second of jitter, far more
 * than the ~100 ms target the plan asks for; in-tree mod_link will use
 * a real adaptive buffer later. */
#define JB_DEPTH 8
typedef struct {
    uint8_t  payload[400];
    int      len;
    uint16_t seq;
    int      have;
} jb_slot_t;
static jb_slot_t g_jb_slots[JB_DEPTH];

/* Newer-than? Treats the 16-bit RTP seq as cyclic. */
static int seq_gt(uint16_t a, uint16_t b)
{
    return (int16_t)(a - b) > 0;
}

static void audio_drain_recv(void)
{
    uint8_t pkt[KERCHUNK_LINK_RTP_MAX_PACKET];
    while (1) {
        ssize_t n = recv(g.udp_fd, pkt, sizeof(pkt), 0);
        if (n <= 0) {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                fprintf(stderr, "recv: %s\n", strerror(errno));
            return;
        }
        g.pkts_recv_total++;
        int len = (int)n;
        if (srtp_unprotect(g.srtp_in, pkt, &len) != srtp_err_status_ok)
            continue;
        g.pkts_recv_authed++;
        if (len <= 12) continue;

        uint16_t seq;
        memcpy(&seq, pkt + 2, 2);
        seq = ntohs(seq);

        if (!g.jb_initialized) {
            g.jb_initialized   = 1;
            g.jb_next_seq      = seq;       /* play this packet first */
            /* Add 100 ms target depth before first playout — the plan's
             * trade-off between latency and jitter tolerance. */
            g.jb_next_play_ms  = now_ms() + 100;
        } else if (!seq_gt(seq, (uint16_t)(g.jb_next_seq - 1))) {
            /* Older than playout cursor → too late, drop. */
            g.frames_late++;
            continue;
        }

        int slot = seq % JB_DEPTH;
        g_jb_slots[slot].seq  = seq;
        g_jb_slots[slot].len  = len - 12;
        g_jb_slots[slot].have = 1;
        memcpy(g_jb_slots[slot].payload, pkt + 12, (size_t)(len - 12));
    }
}

static void audio_playout_tick(void)
{
    if (!g.jb_initialized) return;
    int64_t now = now_ms();
    /* Pull (potentially many) frames at once if we're behind. */
    while (now >= g.jb_next_play_ms) {
        int slot = g.jb_next_seq % JB_DEPTH;
        int16_t pcm[KERCHUNK_LINK_OPUS_FRAME_SAMPLES];
        if (g_jb_slots[slot].have && g_jb_slots[slot].seq == g.jb_next_seq) {
            int dlen = opus_decode(g.opus_dec,
                                   g_jb_slots[slot].payload,
                                   g_jb_slots[slot].len,
                                   pcm, KERCHUNK_LINK_OPUS_FRAME_SAMPLES, 0);
            if (dlen > 0) g.frames_decoded++;
            g_jb_slots[slot].have = 0;
        } else {
            /* Empty / wrong seq → Opus PLC concealment frame. */
            (void)opus_decode(g.opus_dec, NULL, 0, pcm,
                              KERCHUNK_LINK_OPUS_FRAME_SAMPLES, 0);
            g.frames_plc++;
        }
        g.jb_next_seq++;
        g.jb_next_play_ms += KERCHUNK_LINK_OPUS_FRAME_MS;
    }
}

static void on_login_ok_parse_audio(struct mg_str body)
{
    char *ep   = mg_json_get_str(body, "$.rtp_endpoint");
    char *kh   = mg_json_get_str(body, "$.srtp_master_key");
    char *sh   = mg_json_get_str(body, "$.srtp_master_salt");
    long  ns   = mg_json_get_long(body, "$.ssrc",            0);
    long  rs   = mg_json_get_long(body, "$.reflector_ssrc",  0);
    if (ep) snprintf(g.rtp_endpoint, sizeof(g.rtp_endpoint), "%s", ep);
    if (kh && strlen(kh) == 2 * KERCHUNK_LINK_SRTP_KEY_BYTES)
        hex_to_bytes(kh, g.master_key, KERCHUNK_LINK_SRTP_KEY_BYTES);
    if (sh && strlen(sh) == 2 * KERCHUNK_LINK_SRTP_SALT_BYTES)
        hex_to_bytes(sh, g.master_salt, KERCHUNK_LINK_SRTP_SALT_BYTES);
    g.node_ssrc = (uint32_t)ns;
    g.refl_ssrc = (uint32_t)rs;
    free(ep); free(kh); free(sh);
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
    } else if (g.audio_send_ms || g.audio_recv_ms) {
        /* Stay in PINGING-but-quiet — main loop drives audio. Send a
         * PT=99 bootstrap so the reflector learns our UDP source addr
         * without engaging floor logic (otherwise a recv-only node
         * would grab the floor on its bootstrap, blocking real talkers). */
        g.state = PST_PINGING;
        g.audio_started_ms = now_ms();
        audio_send_bootstrap();
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
    } else if (g.audio_send_ms || g.audio_recv_ms) {
        g.state = PST_PINGING;
        g.audio_started_ms = now_ms();
        audio_send_bootstrap();   /* PT=99 — addr-learn, no floor grab */
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
        on_login_ok_parse_audio(body);
        if ((g.audio_send_ms || g.audio_recv_ms) && audio_init() < 0) {
            free(type); return;
        }
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
        /* In control-plane talk modes the test wants us to exit on
         * floor_denied. In audio modes it's normal feedback (we just
         * try the next frame) — keep going. */
        if (!g.audio_send_ms && !g.audio_recv_ms) {
            g.state = PST_DONE;
            c->is_draining = 1;
        }
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
    if (ev == MG_EV_CONNECT && c->is_tls) {
        /* Lab probe — never verify, just establish TLS. */
        struct mg_tls_opts opts = { .skip_verification = 1 };
        mg_tls_init(c, &opts);
        return;
    }
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
        {"tg",            required_argument, NULL, 1000},
        {"talk",          no_argument,       NULL, 1001},
        {"hold-ms",       required_argument, NULL, 1002},
        {"audio-send-ms", required_argument, NULL, 1003},
        {"audio-recv-ms", required_argument, NULL, 1004},
        {0,0,0,0}
    };

    g.udp_fd = -1;

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
        case 1003: g.audio_send_ms = atoi(optarg); g.pings_target = 0; break;
        case 1004: g.audio_recv_ms = atoi(optarg); g.pings_target = 0; break;
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
    int64_t next_audio_send = 0;
    while (g.state != PST_DONE && g.state != PST_FAILED &&
           time(NULL) < deadline) {
        mg_mgr_poll(&mgr, 10);

        /* Audio: drain receives + run the JB playout tick every loop;
         * pace sends at 60 ms cadence. */
        if (g.udp_fd >= 0 && g.audio_started_ms > 0) {
            audio_drain_recv();
            audio_playout_tick();
            int64_t now = now_ms();
            if (g.audio_send_ms > 0 &&
                now - g.audio_started_ms < g.audio_send_ms &&
                now >= next_audio_send) {
                audio_send_one_frame();
                next_audio_send = now + KERCHUNK_LINK_OPUS_FRAME_MS;
            }
            int64_t expected_done_ms =
                g.audio_send_ms > g.audio_recv_ms
                    ? g.audio_send_ms : g.audio_recv_ms;
            if (now - g.audio_started_ms >= expected_done_ms + 200)
                g.state = PST_DONE;
        }
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
        if (g.audio_send_ms || g.audio_recv_ms) {
            printf("AUDIO sent=%d recv=%d authed=%d decoded=%d "
                   "plc=%d late=%d OK\n",
                   g.pkts_sent, g.pkts_recv_total,
                   g.pkts_recv_authed, g.frames_decoded,
                   g.frames_plc, g.frames_late);
        } else {
            printf("OK\n");
        }
        return 0;
    }
    fprintf(stderr, "FAIL — %s\n", g.fail_reason);
    return 1;
}
