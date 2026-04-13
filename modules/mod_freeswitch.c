/*
 * mod_freeswitch.c — AutoPatch telephone interconnect
 *
 * Connects radio users to phone calls via FreeSWITCH ESL + UnicastStream.
 * User dials *0<digits># to originate, *0# to hang up.
 *
 * Audio flow:
 *   Radio→Phone: audio tap → UDP sendto (in audio thread)
 *   Phone→Radio: UDP recvfrom → jitter buffer → VAD → queue_audio_buffer
 *
 * Config: [freeswitch] section in kerchunk.conf
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "kerchunk_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdatomic.h>

#define LOG_MOD "freeswitch"

/* DTMF event offset — *0<digits># or *0# */
#define DTMF_EVT_AUTOPATCH  17

/* VAD defaults */
#define VAD_FRAME_SAMPLES    160   /* 20ms at 8kHz */
#define VAD_THRESHOLD_DEF    800
#define VAD_HOLD_MS_DEF      500
#define VAD_ATTACK_FRAMES    2

/* Jitter buffer */
#define JITTER_BUF_SAMPLES   1600  /* 200ms at 8kHz */

/* ESL receive buffer */
#define ESL_BUF_SIZE         16384

/* Call states */
typedef enum {
    CALL_IDLE,
    CALL_DIALING,
    CALL_RINGING,
    CALL_CONNECTED,
} call_state_t;

/* Jitter buffer (ring buffer) */
typedef struct {
    int16_t buf[JITTER_BUF_SAMPLES];
    size_t  write_pos;
    size_t  read_pos;
    size_t  count;
} jitter_buf_t;

/* ================================================================== */
/*  Static globals                                                     */
/* ================================================================== */

static kerchunk_core_t *g_core;

/* Config */
static char g_fs_host[64]         = "127.0.0.1";
static int  g_fs_esl_port         = 8021;
static char g_fs_esl_password[64] = "ClueCon";
static char g_sip_gateway[64]     = "voip_provider";
static int  g_udp_base_port       = 16000;
static int  g_max_call_secs       = 180;
static int  g_dial_timeout_ms     = 30000;
static int  g_inactivity_ms       = 60000;
static int  g_vad_threshold       = VAD_THRESHOLD_DEF;
static int  g_vad_hold_ms         = VAD_HOLD_MS_DEF;
static int  g_enabled             = 0;
static int  g_admin_only          = 0;
static char g_dial_prefix[16]     = "1";
static char g_dial_whitelist[256] = "";
static char g_sounds_dir[256]     = "./sounds";

/* Call state */
static call_state_t g_call_state   = CALL_IDLE;
static atomic_int g_call_active  = 0;
static char g_call_uuid[64]        = "";
static char g_call_digits[32]      = "";
static int  g_call_timer           = -1;
static int  g_inactivity_timer     = -1;
static int  g_dial_timer           = -1;

/* ESL client */
static int  g_esl_fd              = -1;
static int  g_esl_connected       = 0;
static int  g_esl_authed          = 0;
static char g_esl_buf[ESL_BUF_SIZE];
static int  g_esl_buf_len         = 0;
static int  g_esl_reconnect_ms    = 1000;

/* UDP audio sockets */
static int  g_udp_rx_fd           = -1;   /* receive phone audio */
static int  g_udp_tx_fd           = -1;   /* send radio audio to phone */
static struct sockaddr_in g_fs_udp_addr;
static socklen_t g_fs_udp_addrlen = sizeof(struct sockaddr_in);

/* Audio state */
static atomic_int g_cor_active     = 0;
static atomic_int g_vox_ptt_held   = 0;
static int          g_vad_hold_remaining = 0;
static int          g_speech_frames  = 0;
static jitter_buf_t g_jitter;

/* ================================================================== */
/*  Jitter buffer                                                      */
/* ================================================================== */

static void jitter_buf_reset(jitter_buf_t *jb)
{
    jb->write_pos = 0;
    jb->read_pos  = 0;
    jb->count     = 0;
    memset(jb->buf, 0, sizeof(jb->buf));
}

static void jitter_buf_write(jitter_buf_t *jb, const int16_t *samples, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        jb->buf[jb->write_pos] = samples[i];
        jb->write_pos = (jb->write_pos + 1) % JITTER_BUF_SAMPLES;

        if (jb->count < JITTER_BUF_SAMPLES) {
            jb->count++;
        } else {
            /* Overflow — drop oldest by advancing read_pos */
            jb->read_pos = (jb->read_pos + 1) % JITTER_BUF_SAMPLES;
        }
    }
}

static size_t jitter_buf_read(jitter_buf_t *jb, int16_t *out, size_t n)
{
    size_t read = 0;
    for (size_t i = 0; i < n; i++) {
        if (jb->count == 0) {
            /* Underflow — fill remainder with silence */
            out[i] = 0;
        } else {
            out[i] = jb->buf[jb->read_pos];
            jb->read_pos = (jb->read_pos + 1) % JITTER_BUF_SAMPLES;
            jb->count--;
            read++;
        }
    }
    return read;
}

/* ================================================================== */
/*  VAD engine                                                         */
/* ================================================================== */

static int compute_rms(const int16_t *buf, size_t n)
{
    if (n == 0) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += (int64_t)buf[i] * (int64_t)buf[i];
    return (int)sqrt((double)sum / (double)n);
}

/*
 * Returns 1 if speech detected, 0 if silence.
 * Uses attack counter (consecutive speech frames) and hold timer.
 */
static int vad_process(const int16_t *buf, size_t n)
{
    int rms = compute_rms(buf, n);

    if (rms >= g_vad_threshold) {
        g_speech_frames++;
        g_vad_hold_remaining = g_vad_hold_ms;
        if (g_speech_frames >= VAD_ATTACK_FRAMES)
            return 1;
        /* During attack phase, still holding if previously speaking */
        return (g_vox_ptt_held) ? 1 : 0;
    }

    /* Below threshold — reset attack counter */
    g_speech_frames = 0;

    /* Hold timer — keep "speaking" for a while after last speech */
    if (g_vad_hold_remaining > 0) {
        g_vad_hold_remaining -= KERCHUNK_FRAME_MS;
        if (g_vad_hold_remaining > 0)
            return 1;
    }

    return 0;
}

static void vad_reset(void)
{
    g_speech_frames      = 0;
    g_vad_hold_remaining = 0;
}

/* ================================================================== */
/*  ESL client                                                         */
/* ================================================================== */

static int esl_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int esl_connect(void)
{
    if (g_esl_fd >= 0) return 0;  /* already connected */

    g_esl_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_esl_fd < 0) {
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                     "ESL socket() failed: %s", strerror(errno));
        return -1;
    }

    if (esl_set_nonblocking(g_esl_fd) < 0) {
        close(g_esl_fd);
        g_esl_fd = -1;
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)g_fs_esl_port);
    inet_pton(AF_INET, g_fs_host, &addr.sin_addr);

    int rc = connect(g_esl_fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(g_esl_fd);
        g_esl_fd = -1;
        return -1;
    }

    g_esl_connected = 0;
    g_esl_authed    = 0;
    g_esl_buf_len   = 0;

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                 "ESL connecting to %s:%d", g_fs_host, g_fs_esl_port);
    return 0;
}

static void esl_disconnect(void)
{
    if (g_esl_fd >= 0) {
        close(g_esl_fd);
        g_esl_fd = -1;
    }
    g_esl_connected = 0;
    g_esl_authed    = 0;
    g_esl_buf_len   = 0;
}

static int esl_send(const char *cmd)
{
    if (g_esl_fd < 0) return -1;
    size_t len = strlen(cmd);
    ssize_t n = send(g_esl_fd, cmd, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    return (n == (ssize_t)len) ? 0 : -1;
}

static int esl_api(const char *command)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "api %s\n\n", command);
    return esl_send(buf);
}

static int esl_bgapi(const char *command)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "bgapi %s\n\n", command);
    return esl_send(buf);
}

/* Forward declarations for call state machine */
static void call_teardown(void);
static void call_setup_audio(void);

/*
 * Parse a key from ESL event text.
 * Scans for "Key: Value\n" and copies value into out.
 */
static int esl_parse_header(const char *text, const char *key,
                            char *out, size_t max)
{
    size_t klen = strlen(key);
    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        /* Verify it's at start of line */
        if (p != text && *(p - 1) != '\n') {
            p += klen;
            continue;
        }
        p += klen;
        if (*p != ':') continue;
        p++;
        while (*p == ' ') p++;
        const char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        /* Strip \r */
        const char *cr = end;
        if (cr > p && *(cr - 1) == '\r') cr--;
        size_t vlen = (size_t)(cr - p);
        if (vlen >= max) vlen = max - 1;
        memcpy(out, p, vlen);
        out[vlen] = '\0';
        return 0;
    }
    return -1;
}

/*
 * Handle a complete ESL event/response block.
 */
static void esl_handle_event(const char *block)
{
    /* Auth request */
    if (strstr(block, "auth/request")) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "auth %s\n\n", g_fs_esl_password);
        esl_send(cmd);
        return;
    }

    /* Auth reply */
    if (strstr(block, "command/reply") && strstr(block, "+OK")) {
        if (!g_esl_authed) {
            g_esl_authed = 1;
            g_esl_connected = 1;
            g_esl_reconnect_ms = 1000;
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "ESL authenticated");

            /* Subscribe to events */
            esl_send("event plain CHANNEL_ANSWER CHANNEL_HANGUP "
                     "CHANNEL_PROGRESS DTMF\n\n");
        }
        return;
    }

    /* Look for Event-Name header */
    char event_name[64] = "";
    esl_parse_header(block, "Event-Name", event_name, sizeof(event_name));

    if (event_name[0] == '\0')
        return;

    /* CHANNEL_ANSWER */
    if (strcmp(event_name, "CHANNEL_ANSWER") == 0) {
        char uuid[64] = "";
        esl_parse_header(block, "Unique-ID", uuid, sizeof(uuid));
        if (uuid[0] == '\0')
            esl_parse_header(block, "Channel-Call-UUID", uuid, sizeof(uuid));

        if (g_call_state == CALL_DIALING || g_call_state == CALL_RINGING) {
            if (g_call_uuid[0] == '\0' || strcmp(uuid, g_call_uuid) == 0 ||
                g_call_uuid[0] == '\0') {
                if (g_call_uuid[0] == '\0')
                    snprintf(g_call_uuid, sizeof(g_call_uuid), "%s", uuid);
                g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                             "call answered: %s", g_call_uuid);
                g_call_state = CALL_CONNECTED;
                call_setup_audio();
            }
        }
        return;
    }

    /* CHANNEL_PROGRESS — ringing indication */
    if (strcmp(event_name, "CHANNEL_PROGRESS") == 0) {
        if (g_call_state == CALL_DIALING) {
            g_call_state = CALL_RINGING;
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "remote ringing");
            char path[512];
            snprintf(path, sizeof(path), "%s/phone/phone_ringing.wav", g_sounds_dir);
            g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
        }
        return;
    }

    /* CHANNEL_HANGUP */
    if (strcmp(event_name, "CHANNEL_HANGUP") == 0) {
        char uuid[64] = "";
        esl_parse_header(block, "Unique-ID", uuid, sizeof(uuid));
        if (uuid[0] == '\0')
            esl_parse_header(block, "Channel-Call-UUID", uuid, sizeof(uuid));

        if (g_call_state != CALL_IDLE &&
            (g_call_uuid[0] == '\0' || strcmp(uuid, g_call_uuid) == 0)) {

            char cause[64] = "";
            esl_parse_header(block, "Hangup-Cause", cause, sizeof(cause));
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                         "call hangup: %s cause=%s", uuid, cause);

            /* Play appropriate prompt based on cause */
            char path[512];
            if (strstr(cause, "USER_BUSY")) {
                snprintf(path, sizeof(path), "%s/phone/phone_busy.wav", g_sounds_dir);
                g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
            } else if (strstr(cause, "NO_ANSWER") || strstr(cause, "NO_USER_RESPONSE")) {
                snprintf(path, sizeof(path), "%s/phone/phone_no_answer.wav", g_sounds_dir);
                g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
            } else if (g_call_state == CALL_CONNECTED) {
                snprintf(path, sizeof(path), "%s/phone/phone_disconnected.wav", g_sounds_dir);
                g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
            } else {
                snprintf(path, sizeof(path), "%s/phone/phone_error.wav", g_sounds_dir);
                g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
            }

            call_teardown();
        }
        return;
    }
}

/*
 * Process incoming ESL data — parse double-newline-delimited blocks.
 */
static void esl_process_buffer(void)
{
    while (g_esl_buf_len > 0) {
        /* Look for double newline (event delimiter) */
        char *sep = strstr(g_esl_buf, "\n\n");
        if (!sep) break;

        size_t block_len = (size_t)(sep - g_esl_buf) + 2;
        *sep = '\0';

        esl_handle_event(g_esl_buf);

        /* Shift remaining data */
        size_t remaining = (size_t)g_esl_buf_len - block_len;
        if (remaining > 0)
            memmove(g_esl_buf, g_esl_buf + block_len, remaining);
        g_esl_buf_len = (int)remaining;
    }
}

/*
 * Poll ESL socket — called from tick handler (20ms).
 */
static void esl_poll(void)
{
    if (g_esl_fd < 0) {
        /* Not connected — attempt reconnect */
        if (!g_enabled) return;
        /* Use static counter for backoff instead of a timer */
        static int backoff_ticks = 0;
        if (backoff_ticks > 0) {
            backoff_ticks--;
            return;
        }
        if (esl_connect() < 0) {
            /* Exponential backoff: 1s, 2s, 4s, ... up to 30s */
            backoff_ticks = g_esl_reconnect_ms / KERCHUNK_FRAME_MS;
            g_esl_reconnect_ms *= 2;
            if (g_esl_reconnect_ms > 30000) g_esl_reconnect_ms = 30000;
        }
        return;
    }

    /* Check if non-blocking connect completed */
    if (!g_esl_connected && !g_esl_authed) {
        /* Try reading — if connect succeeded, FS sends auth/request */
    }

    /* Receive data */
    ssize_t n = recv(g_esl_fd, g_esl_buf + g_esl_buf_len,
                     (size_t)(ESL_BUF_SIZE - 1 - g_esl_buf_len), MSG_DONTWAIT);
    if (n > 0) {
        g_esl_buf_len += (int)n;
        g_esl_buf[g_esl_buf_len] = '\0';
        esl_process_buffer();
        /* Safety: if the buffer is full without a complete event
         * delimiter ("\n\n"), flush it to avoid a stuck state. */
        if (g_esl_buf_len >= ESL_BUF_SIZE - 1) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "ESL buffer full (%d bytes) without delimiter — flushing",
                        g_esl_buf_len);
            g_esl_buf_len = 0;
            g_esl_buf[0] = '\0';
        }
    } else if (n == 0) {
        /* Connection closed */
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "ESL connection closed");
        esl_disconnect();
        if (g_call_state != CALL_IDLE)
            call_teardown();
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        /* Real error */
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                     "ESL recv error: %s", strerror(errno));
        esl_disconnect();
        if (g_call_state != CALL_IDLE)
            call_teardown();
    }
}

/* ================================================================== */
/*  UDP audio transport                                                */
/* ================================================================== */

static int udp_open(void)
{
    /* RX socket (receive phone audio) */
    g_udp_rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_rx_fd < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                     "UDP RX socket failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family      = AF_INET;
    rx_addr.sin_addr.s_addr = INADDR_ANY;
    rx_addr.sin_port        = htons((uint16_t)g_udp_base_port);

    if (bind(g_udp_rx_fd, (struct sockaddr *)&rx_addr, sizeof(rx_addr)) < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                     "UDP RX bind port %d failed: %s",
                     g_udp_base_port, strerror(errno));
        close(g_udp_rx_fd);
        g_udp_rx_fd = -1;
        return -1;
    }

    /* Set non-blocking */
    esl_set_nonblocking(g_udp_rx_fd);

    /* TX socket (send radio audio to phone) */
    g_udp_tx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_tx_fd < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                     "UDP TX socket failed: %s", strerror(errno));
        close(g_udp_rx_fd);
        g_udp_rx_fd = -1;
        return -1;
    }

    /* Set up FreeSWITCH target address for TX */
    memset(&g_fs_udp_addr, 0, sizeof(g_fs_udp_addr));
    g_fs_udp_addr.sin_family = AF_INET;
    g_fs_udp_addr.sin_port   = htons((uint16_t)(g_udp_base_port + 1));
    inet_pton(AF_INET, g_fs_host, &g_fs_udp_addr.sin_addr);
    g_fs_udp_addrlen = sizeof(g_fs_udp_addr);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                 "UDP audio: rx=%d tx=%d (base port %d)",
                 g_udp_rx_fd, g_udp_tx_fd, g_udp_base_port);
    return 0;
}

static void udp_close(void)
{
    if (g_udp_rx_fd >= 0) {
        close(g_udp_rx_fd);
        g_udp_rx_fd = -1;
    }
    if (g_udp_tx_fd >= 0) {
        close(g_udp_tx_fd);
        g_udp_tx_fd = -1;
    }
}

/*
 * Audio tap — runs in audio thread.
 * Sends captured radio RX audio to FreeSWITCH via UDP.
 * Only active when call is connected AND COR is active.
 */
static int g_tx_pkt_count = 0;

/*
 * Playback tap — sends repeater TX audio (CW ID, TTS, weather, courtesy
 * tones, relayed audio) to the phone caller so they hear everything the
 * repeater transmits.
 */
static void playback_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_call_active || g_udp_tx_fd < 0)
        return;

    /* Don't echo back audio that came FROM the phone (VOX→queue→playback) */
    if (g_vox_ptt_held)
        return;

    int src_rate = g_core->sample_rate;
    if (src_rate > 8000) {
        int ratio = src_rate / 8000;
        size_t out_n = evt->audio.n / (size_t)ratio;
        int16_t ds[960];
        if (out_n > sizeof(ds)/sizeof(ds[0])) out_n = sizeof(ds)/sizeof(ds[0]);
        for (size_t i = 0; i < out_n; i++)
            ds[i] = evt->audio.samples[i * ratio];
        sendto(g_udp_tx_fd, ds, out_n * sizeof(int16_t), MSG_DONTWAIT,
               (struct sockaddr *)&g_fs_udp_addr, g_fs_udp_addrlen);
    } else {
        sendto(g_udp_tx_fd, evt->audio.samples,
               evt->audio.n * sizeof(int16_t), MSG_DONTWAIT,
               (struct sockaddr *)&g_fs_udp_addr, g_fs_udp_addrlen);
    }
}

/*
 * RX audio tap — sends captured radio RX audio to the phone caller.
 * Only active when COR is active (someone keying up on RF).
 */
static void radio_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_call_active || !g_cor_active || g_udp_tx_fd < 0)
        return;

    /* Downsample from pipeline rate (48kHz) to FS rate (8kHz SLIN) */
    int src_rate = g_core->sample_rate;
    if (src_rate > 8000) {
        int ratio = src_rate / 8000;
        size_t out_n = evt->audio.n / (size_t)ratio;
        int16_t ds[960]; /* max 48000/50 / 6 = 160 */
        if (out_n > sizeof(ds)/sizeof(ds[0])) out_n = sizeof(ds)/sizeof(ds[0]);
        for (size_t i = 0; i < out_n; i++)
            ds[i] = evt->audio.samples[i * ratio];
        ssize_t sent = sendto(g_udp_tx_fd, ds, out_n * sizeof(int16_t), MSG_DONTWAIT,
               (struct sockaddr *)&g_fs_udp_addr, g_fs_udp_addrlen);
        g_tx_pkt_count++;
        if (g_tx_pkt_count <= 10 || g_tx_pkt_count % 100 == 0)
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                        "UDP TX #%d: %zu samples@%dHz → %zu samples@8kHz (%zd bytes sent to port %d)",
                        g_tx_pkt_count, evt->audio.n, src_rate, out_n,
                        sent, ntohs(g_fs_udp_addr.sin_port));
    } else {
        ssize_t sent = sendto(g_udp_tx_fd, evt->audio.samples,
               evt->audio.n * sizeof(int16_t), MSG_DONTWAIT,
               (struct sockaddr *)&g_fs_udp_addr, g_fs_udp_addrlen);
        g_tx_pkt_count++;
        if (g_tx_pkt_count <= 10 || g_tx_pkt_count % 100 == 0)
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                        "UDP TX #%d: %zu samples (%zd bytes sent to port %d)",
                        g_tx_pkt_count, evt->audio.n, sent,
                        ntohs(g_fs_udp_addr.sin_port));
    }
}

/*
 * Receive phone audio from UDP into jitter buffer.
 * Called from tick handler (main thread, 20ms).
 */
static void udp_receive_audio(void)
{
    int16_t pkt[320];  /* Up to 40ms per packet */
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    /* Drain all available packets */
    static int rx_pkt_count = 0;
    static int rx_poll_count = 0;
    rx_poll_count++;

    /* Log every second that we're polling (50 ticks = 1 sec) */
    if (rx_poll_count % 50 == 0)
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                    "UDP poll #%d: fd=%d port=%d total_pkts=%d",
                    rx_poll_count, g_udp_rx_fd, g_udp_base_port, rx_pkt_count);

    for (int i = 0; i < 10; i++) {
        ssize_t n = recvfrom(g_udp_rx_fd, pkt, sizeof(pkt), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) break;
        size_t samples = (size_t)n / sizeof(int16_t);
        if (samples > 0) {
            jitter_buf_write(&g_jitter, pkt, samples);
            rx_pkt_count++;
            if (rx_pkt_count <= 10 || rx_pkt_count % 100 == 0) {
                uint8_t *ip = (uint8_t *)&from.sin_addr;
                g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                            "UDP RX #%d: %zd bytes (%zu samples) from %d.%d.%d.%d:%d",
                            rx_pkt_count, n, samples,
                            ip[0], ip[1], ip[2], ip[3], ntohs(from.sin_port));
            }
        }
    }
}

/* ================================================================== */
/*  Call state machine                                                 */
/* ================================================================== */

/* Forward declarations for timer callbacks */
static void on_call_timeout(void *ud);
static void on_dial_timeout(void *ud);
static void on_inactivity_timeout(void *ud);

/*
 * Check if a phone number is allowed to be dialed.
 * Blocks 911 calls (SIP trunks lack E911 location data).
 * Enforces whitelist if configured.
 */
static int is_number_allowed(const char *digits)
{
    if (!digits || digits[0] == '\0')
        return 0;

    /* Block 911 — SIP trunks lack E911 location data */
    if (strcmp(digits, "911") == 0 || strcmp(digits, "9911") == 0 ||
        strcmp(digits, "1911") == 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                     "911 blocked — SIP trunk lacks E911 location data");
        return 0;
    }

    /* Minimum length check */
    size_t len = strlen(digits);
    if (len < 3) return 0;

    /* Verify all digits */
    for (size_t i = 0; i < len; i++) {
        if (digits[i] < '0' || digits[i] > '9')
            return 0;
    }

    /* Whitelist check — if configured, area code prefix must match */
    if (g_dial_whitelist[0] != '\0') {
        /* Parse comma-separated prefixes */
        char wl[256];
        snprintf(wl, sizeof(wl), "%s", g_dial_whitelist);
        char *save = NULL;
        char *tok = strtok_r(wl, ",", &save);
        int found = 0;
        while (tok) {
            /* Strip leading whitespace */
            while (*tok == ' ') tok++;
            size_t tlen = strlen(tok);
            if (tlen > 0 && len >= tlen && strncmp(digits, tok, tlen) == 0) {
                found = 1;
                break;
            }
            tok = strtok_r(NULL, ",", &save);
        }
        if (!found) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                         "number %s not on whitelist", digits);
            return 0;
        }
    }

    return 1;
}

/*
 * Originate a phone call via FreeSWITCH ESL.
 */
static void autopatch_dial(const char *digits)
{
    if (!g_enabled) {
        char path[512];
        snprintf(path, sizeof(path), "%s/phone/phone_not_available.wav", g_sounds_dir);
        g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
        return;
    }

    if (g_call_state != CALL_IDLE) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                     "dial rejected — call already active");
        return;
    }

    if (!g_esl_authed) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                     "dial rejected — ESL not connected");
        char path[512];
        snprintf(path, sizeof(path), "%s/phone/phone_error.wav", g_sounds_dir);
        g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
        return;
    }

    /* Admin-only check */
    if (g_admin_only) {
        /* For now, require emergency override or admin access.
         * In a full implementation, check the current caller's access level. */
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                     "dial rejected — admin only mode");
        char path[512];
        snprintf(path, sizeof(path), "%s/phone/phone_access_denied.wav", g_sounds_dir);
        g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
        return;
    }

    /* Validate number */
    if (!is_number_allowed(digits)) {
        char path[512];
        snprintf(path, sizeof(path), "%s/phone/phone_error.wav", g_sounds_dir);
        g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
        return;
    }

    /* Store digits */
    snprintf(g_call_digits, sizeof(g_call_digits), "%s", digits);
    g_call_uuid[0] = '\0';

    /* Only prepend the configured dial prefix if the number doesn't
     * already start with it. Callers can type either 10-digit or 11-
     * digit NANPA numbers and get the same result — no more double
     * country-code bug. */
    const char *effective_prefix = g_dial_prefix;
    if (g_dial_prefix[0] != '\0') {
        size_t plen = strlen(g_dial_prefix);
        if (strncmp(digits, g_dial_prefix, plen) == 0)
            effective_prefix = "";
    }

    /* Build originate command */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "originate {ignore_early_media=false}sofia/gateway/%s/%s%s &park()",
             g_sip_gateway, effective_prefix, digits);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                 "dialing %s%s via %s (cmd: %s)",
                 effective_prefix, digits, g_sip_gateway, cmd);

    if (esl_bgapi(cmd) < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "ESL bgapi failed");
        char path[512];
        snprintf(path, sizeof(path), "%s/phone/phone_error.wav", g_sounds_dir);
        g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);
        return;
    }

    g_call_state = CALL_DIALING;

    /* Play dialing prompt */
    char path[512];
    snprintf(path, sizeof(path), "%s/phone/phone_dialing.wav", g_sounds_dir);
    g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);

    /* Start dial timeout */
    g_dial_timer = g_core->timer_create(g_dial_timeout_ms, 0,
                                         on_dial_timeout, NULL);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "freeswitch", .description = "dialing" } };
    g_core->fire_event(&ae);
}

/*
 * Hang up the current call.
 */
static void autopatch_hangup(void)
{
    if (g_call_state == CALL_IDLE) {
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "hangup — no active call");
        return;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                 "hangup requested, state=%d uuid=%s",
                 g_call_state, g_call_uuid);

    /* Kill the call via ESL if we have a UUID */
    if (g_call_uuid[0] != '\0') {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "uuid_kill %s", g_call_uuid);
        esl_api(cmd);
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/phone/phone_disconnected.wav", g_sounds_dir);
    g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);

    call_teardown();
}

/*
 * Set up audio bridge after CHANNEL_ANSWER.
 */
static void call_setup_audio(void)
{
    /* Open UDP sockets */
    if (udp_open() < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                     "failed to open UDP sockets — tearing down");
        autopatch_hangup();
        return;
    }

    /* Tell FreeSWITCH to send/receive audio via UDP unicast.
     * Uses ESL sendmsg with call-command: unicast (same protocol as socket2me).
     * Without "flags: native", FS sends SLIN (signed linear 16-bit) at 8kHz.
     * We resample between 8kHz (FS) and our pipeline rate (typically 48kHz).
     *
     * From FS's perspective:
     *   local-ip/port  = where FS BINDS (receives from us)
     *   remote-ip/port = where FS SENDS to (we receive)
     *
     * So: FS binds on base+1 (we sendto base+1), FS sends to base (we recvfrom base) */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "sendmsg %s\n"
             "call-command: unicast\n"
             "local-ip: %s\n"
             "local-port: %d\n"
             "remote-ip: %s\n"
             "remote-port: %d\n"
             "transport: udp\n"
             "\n",
             g_call_uuid,
             g_fs_host, g_udp_base_port + 1,  /* FS binds here (we sendto here) */
             g_fs_host, g_udp_base_port);      /* FS sends here (we recvfrom here) */

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                "unicast: FS binds %s:%d, sends to %s:%d (uuid=%s)",
                g_fs_host, g_udp_base_port + 1,
                g_fs_host, g_udp_base_port,
                g_call_uuid);
    esl_send(cmd);

    /* Register audio taps:
     *   RX tap: radio inbound audio → phone (when COR active)
     *   Playback tap: repeater TX audio → phone (CW ID, TTS, etc.) */
    g_core->audio_tap_register(radio_audio_tap, NULL);
    g_core->playback_tap_register(playback_audio_tap, NULL);

    /* Reset jitter buffer and VAD */
    jitter_buf_reset(&g_jitter);
    vad_reset();
    g_vox_ptt_held = 0;

    /* Mark call as active (volatile — read by audio thread tap) */
    g_call_active = 1;

    /* Cancel dial timer, start call duration timer */
    if (g_dial_timer >= 0) {
        g_core->timer_cancel(g_dial_timer);
        g_dial_timer = -1;
    }
    g_call_timer = g_core->timer_create(g_max_call_secs * 1000, 0,
                                         on_call_timeout, NULL);

    /* Start inactivity timer */
    g_inactivity_timer = g_core->timer_create(g_inactivity_ms, 0,
                                               on_inactivity_timeout, NULL);

    /* Play connected prompt */
    char path[512];
    snprintf(path, sizeof(path), "%s/phone/phone_connected.wav", g_sounds_dir);
    g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                 "call connected, audio bridge active");
}

/*
 * Tear down everything — timers, audio tap, UDP, PTT, state.
 */
static void call_teardown(void)
{
    /* Cancel all timers */
    if (g_call_timer >= 0) {
        g_core->timer_cancel(g_call_timer);
        g_call_timer = -1;
    }
    if (g_dial_timer >= 0) {
        g_core->timer_cancel(g_dial_timer);
        g_dial_timer = -1;
    }
    if (g_inactivity_timer >= 0) {
        g_core->timer_cancel(g_inactivity_timer);
        g_inactivity_timer = -1;
    }

    /* Deactivate call (volatile — blocks audio thread tap) */
    g_call_active = 0;

    /* Unregister audio tap */
    g_core->audio_tap_unregister(radio_audio_tap);
    g_core->playback_tap_unregister(playback_audio_tap);

    /* Close UDP sockets */
    udp_close();

    /* Release PTT if held */
    if (g_vox_ptt_held) {
        g_core->release_ptt("freeswitch");
        g_vox_ptt_held = 0;
    }

    /* Reset state */
    g_call_state    = CALL_IDLE;
    g_call_uuid[0]  = '\0';
    g_call_digits[0] = '\0';
    jitter_buf_reset(&g_jitter);
    vad_reset();

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "call teardown complete");

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "freeswitch", .description = "call ended" } };
    g_core->fire_event(&ae);
}

/* ================================================================== */
/*  Timer callbacks                                                    */
/* ================================================================== */

static void on_call_timeout(void *ud)
{
    (void)ud;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                 "max call duration reached (%ds)", g_max_call_secs);
    g_call_timer = -1;

    /* Play timeout prompt then hang up */
    char path[512];
    snprintf(path, sizeof(path), "%s/phone/phone_timeout.wav", g_sounds_dir);
    g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);

    /* Kill via ESL */
    if (g_call_uuid[0] != '\0') {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "uuid_kill %s", g_call_uuid);
        esl_api(cmd);
    }

    call_teardown();
}

static void on_dial_timeout(void *ud)
{
    (void)ud;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                 "dial timeout — no answer in %dms", g_dial_timeout_ms);
    g_dial_timer = -1;

    char path[512];
    snprintf(path, sizeof(path), "%s/phone/phone_no_answer.wav", g_sounds_dir);
    g_core->queue_audio_file(path, KERCHUNK_PRI_ELEVATED);

    if (g_call_uuid[0] != '\0') {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "uuid_kill %s", g_call_uuid);
        esl_api(cmd);
    }

    call_teardown();
}

static void on_inactivity_timeout(void *ud)
{
    (void)ud;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                 "inactivity timeout — no radio activity in %dms",
                 g_inactivity_ms);
    g_inactivity_timer = -1;

    autopatch_hangup();
}

/* ================================================================== */
/*  COR gating                                                         */
/* ================================================================== */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_cor_active = 1;

    /* Reset inactivity timer when radio user keys up */
    if (g_call_active && g_inactivity_timer >= 0) {
        g_core->timer_cancel(g_inactivity_timer);
        g_inactivity_timer = g_core->timer_create(g_inactivity_ms, 0,
                                                    on_inactivity_timeout, NULL);
    }
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_cor_active = 0;
}

/* ================================================================== */
/*  VOX processing (phone→radio)                                       */
/* ================================================================== */

/*
 * Read from jitter buffer, run VAD, manage PTT and queue audio.
 * Called from tick handler when call is active and COR is inactive.
 */
static int g_vox_debug_count = 0;

static void vox_process_and_queue(void)
{
    int16_t frame[VAD_FRAME_SAMPLES];  /* 8kHz frame from FS (160 samples = 20ms) */
    size_t got = jitter_buf_read(&g_jitter, frame, VAD_FRAME_SAMPLES);

    g_vox_debug_count++;
    if (g_vox_debug_count <= 10 || g_vox_debug_count % 250 == 0) {
        /* Compute RMS for debug */
        int64_t pwr = 0;
        for (size_t k = 0; k < (got > 0 ? got : 1); k++)
            pwr += (int64_t)frame[k] * frame[k];
        int rms = got > 0 ? (int)sqrt((double)pwr / got) : 0;
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                    "VOX #%d: jitter_read=%zu samples, rms=%d, threshold=%d, cor_active=%d",
                    g_vox_debug_count, got, rms, g_vad_threshold, (int)g_cor_active);
    }

    /* If no real data, treat as silence */
    int speaking = 0;
    if (got > 0)
        speaking = vad_process(frame, VAD_FRAME_SAMPLES);

    if (speaking) {
        if (!g_vox_ptt_held) {
            g_core->request_ptt("freeswitch");
            g_vox_ptt_held = 1;
        }
        /* Upsample from 8kHz (FS) to pipeline rate (48kHz) before queuing */
        int dst_rate = g_core->sample_rate;
        if (dst_rate > 8000) {
            int ratio = dst_rate / 8000;
            size_t out_n = VAD_FRAME_SAMPLES * (size_t)ratio;
            int16_t us[960]; /* max 160 * 6 = 960 */
            if (out_n > sizeof(us)/sizeof(us[0])) out_n = sizeof(us)/sizeof(us[0]);
            for (size_t i = 0; i < out_n; i++) {
                size_t src_idx = i / (size_t)ratio;
                size_t frac = i % (size_t)ratio;
                if (src_idx + 1 < VAD_FRAME_SAMPLES) {
                    int32_t a = frame[src_idx];
                    int32_t b = frame[src_idx + 1];
                    us[i] = (int16_t)(a + (b - a) * (int32_t)frac / ratio);
                } else {
                    us[i] = frame[VAD_FRAME_SAMPLES - 1];
                }
            }
            kerchunk_queue_add_buffer_src(us, out_n,
                                          KERCHUNK_PRI_ELEVATED, 0, "phone");
        } else {
            kerchunk_queue_add_buffer_src(frame, VAD_FRAME_SAMPLES,
                                          KERCHUNK_PRI_ELEVATED, 0, "phone");
        }
    } else {
        if (g_vox_ptt_held) {
            g_core->release_ptt("freeswitch");
            g_vox_ptt_held = 0;
        }
    }
}

/* ================================================================== */
/*  DTMF handler                                                       */
/* ================================================================== */

static void on_autopatch(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    const char *arg = (const char *)evt->custom.data;

    if (!arg || arg[0] == '\0') {
        autopatch_hangup();
    } else {
        autopatch_dial(arg);
    }
}

/* ================================================================== */
/*  Tick handler                                                       */
/* ================================================================== */

static void on_tick(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;

    /* 1. Poll ESL for events/responses */
    esl_poll();

    /* 2. Receive phone audio from UDP */
    if (g_call_active && g_udp_rx_fd >= 0)
        udp_receive_audio();

    /* 3. VAD + queue phone audio to repeater (only when COR inactive) */
    if (g_call_active && !g_cor_active)
        vox_process_and_queue();
}

/* ================================================================== */
/*  Shutdown handler                                                   */
/* ================================================================== */

static void on_shutdown(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_call_state != CALL_IDLE)
        autopatch_hangup();
}

/* ================================================================== */
/*  Module lifecycle                                                   */
/* ================================================================== */

static int freeswitch_load(kerchunk_core_t *core)
{
    g_core = core;

    if (core->dtmf_register)
        core->dtmf_register("0", DTMF_EVT_AUTOPATCH, "AutoPatch", "autopatch");

    core->subscribe(KERCHEVT_CUSTOM + DTMF_EVT_AUTOPATCH,
                    on_autopatch, NULL);
    core->subscribe(KERCHEVT_COR_ASSERT, on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,   on_cor_drop,   NULL);
    core->subscribe(KERCHEVT_TICK,       on_tick,       NULL);
    core->subscribe(KERCHEVT_SHUTDOWN,   on_shutdown,   NULL);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "loaded");
    return 0;
}

static int freeswitch_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "freeswitch", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "freeswitch", "freeswitch_host");
    if (v) snprintf(g_fs_host, sizeof(g_fs_host), "%s", v);

    g_fs_esl_port = kerchunk_config_get_int(cfg, "freeswitch", "esl_port", 8021);

    v = kerchunk_config_get(cfg, "freeswitch", "esl_password");
    if (v) snprintf(g_fs_esl_password, sizeof(g_fs_esl_password), "%s", v);

    v = kerchunk_config_get(cfg, "freeswitch", "sip_gateway");
    if (v) snprintf(g_sip_gateway, sizeof(g_sip_gateway), "%s", v);

    g_udp_base_port  = kerchunk_config_get_int(cfg, "freeswitch", "udp_base_port", 16000);
    g_max_call_secs  = kerchunk_config_get_duration_s(cfg, "freeswitch", "max_call_duration", 180);
    g_dial_timeout_ms = kerchunk_config_get_duration_ms(cfg, "freeswitch", "dial_timeout", 30000);
    g_inactivity_ms  = kerchunk_config_get_duration_ms(cfg, "freeswitch", "inactivity_timeout", 60000);
    g_vad_threshold  = kerchunk_config_get_int(cfg, "freeswitch", "vad_threshold", VAD_THRESHOLD_DEF);
    g_vad_hold_ms    = kerchunk_config_get_duration_ms(cfg, "freeswitch", "vad_hold_ms", VAD_HOLD_MS_DEF);

    v = kerchunk_config_get(cfg, "freeswitch", "admin_only");
    g_admin_only = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "freeswitch", "dial_prefix");
    if (v) snprintf(g_dial_prefix, sizeof(g_dial_prefix), "%s", v);

    v = kerchunk_config_get(cfg, "freeswitch", "dial_whitelist");
    if (v) snprintf(g_dial_whitelist, sizeof(g_dial_whitelist), "%s", v);

    v = kerchunk_config_get(cfg, "general", "sounds_dir");
    if (v) snprintf(g_sounds_dir, sizeof(g_sounds_dir), "%s", v);

    /* Clamp values */
    if (g_max_call_secs < 30)   g_max_call_secs  = 30;
    if (g_max_call_secs > 3600) g_max_call_secs  = 3600;
    if (g_vad_threshold < 100)  g_vad_threshold   = 100;
    if (g_vad_threshold > 5000) g_vad_threshold   = 5000;
    if (g_vad_hold_ms < 100)    g_vad_hold_ms     = 100;
    if (g_vad_hold_ms > 5000)   g_vad_hold_ms     = 5000;

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                 "enabled=%d host=%s esl_port=%d gateway=%s "
                 "udp_base=%d max_call=%ds dial_timeout=%dms "
                 "inactivity=%dms vad_thresh=%d vad_hold=%dms "
                 "admin_only=%d prefix=%s",
                 g_enabled, g_fs_host, g_fs_esl_port, g_sip_gateway,
                 g_udp_base_port, g_max_call_secs, g_dial_timeout_ms,
                 g_inactivity_ms, g_vad_threshold, g_vad_hold_ms,
                 g_admin_only, g_dial_prefix);

    return 0;
}

static void freeswitch_unload(void)
{
    /* Tear down active call */
    if (g_call_state != CALL_IDLE)
        call_teardown();

    /* Disconnect ESL */
    esl_disconnect();

    /* Unsubscribe */
    if (g_core->dtmf_unregister)
        g_core->dtmf_unregister("0");
    g_core->unsubscribe(KERCHEVT_CUSTOM + DTMF_EVT_AUTOPATCH, on_autopatch);
    g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,   on_cor_drop);
    g_core->unsubscribe(KERCHEVT_TICK,       on_tick);
    g_core->unsubscribe(KERCHEVT_SHUTDOWN,   on_shutdown);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded");
}

/* ================================================================== */
/*  CLI                                                                */
/* ================================================================== */

static int cli_freeswitch(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    if (argc >= 3 && strcmp(argv[1], "dial") == 0) {
        autopatch_dial(argv[2]);
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "dialing");
        resp_str(r, "number", argv[2]);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "hangup") == 0) {
        autopatch_hangup();
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "hangup");
        return 0;
    }

    /* Status display */
    resp_bool(r, "enabled", g_enabled);
    resp_bool(r, "esl_connected", g_esl_connected);
    resp_bool(r, "esl_authed", g_esl_authed);
    resp_str(r, "host", g_fs_host);
    resp_int(r, "esl_port", g_fs_esl_port);
    resp_str(r, "gateway", g_sip_gateway);
    resp_int(r, "udp_base_port", g_udp_base_port);
    resp_int(r, "max_call_secs", g_max_call_secs);
    resp_bool(r, "admin_only", g_admin_only);

    const char *state_str = "idle";
    switch (g_call_state) {
        case CALL_IDLE:      state_str = "idle";      break;
        case CALL_DIALING:   state_str = "dialing";   break;
        case CALL_RINGING:   state_str = "ringing";   break;
        case CALL_CONNECTED: state_str = "connected"; break;
    }
    resp_str(r, "call_state", state_str);

    if (g_call_state != CALL_IDLE) {
        resp_str(r, "uuid", g_call_uuid);
        resp_str(r, "digits", g_call_digits);
        resp_bool(r, "cor_active", g_cor_active);
        resp_bool(r, "vox_ptt_held", g_vox_ptt_held);
    }

    return 0;

usage:
    resp_text_raw(r, "AutoPatch telephone interconnect via FreeSWITCH\n\n"
        "  freeswitch\n"
        "    Show autopatch status: ESL connection, call state, gateway,\n"
        "    active call UUID and digits, VAD/PTT state.\n\n"
        "  freeswitch dial <number>\n"
        "    Originate a phone call to the given number.\n"
        "    number: digits to dial via the configured SIP gateway\n\n"
        "  freeswitch hangup\n"
        "    Hang up the active call.\n\n"
        "    Audio flow: Radio->Phone via UDP audio tap,\n"
        "    Phone->Radio via UDP recvfrom with jitter buffer and VAD.\n"
        "    DTMF *0<digits># to originate, *0# to hang up.\n\n"
        "Config: [freeswitch] enabled, host, esl_port, esl_password,\n"
        "        sip_gateway, udp_base_port, max_call_secs,\n"
        "        dial_timeout, inactivity_timeout, vad_threshold,\n"
        "        vad_hold_ms, admin_only, dial_prefix, dial_whitelist\n");
    resp_str(r, "error", "usage: freeswitch [dial <num>|hangup]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "freeswitch", .usage = "freeswitch [dial <num>|hangup]",
      .description = "AutoPatch status/control", .handler = cli_freeswitch, .category = "Audio",
      .subcommands = "dial,hangup" },
};

static kerchunk_module_def_t mod_freeswitch = {
    .name             = "mod_freeswitch",
    .version          = "1.0.0",
    .description      = "AutoPatch telephone interconnect via FreeSWITCH",
    .load             = freeswitch_load,
    .configure        = freeswitch_configure,
    .unload           = freeswitch_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_freeswitch);
