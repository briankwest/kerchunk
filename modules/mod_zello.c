/*
 * mod_zello.c — Zello Channel API bridge
 *
 * Runs a libzello client inside kerchunk, connecting to a Zello channel
 * (Friends & Family or Zello Work) and bridging audio between Zello and
 * RF.
 *
 * Audio profile is libzello's default: Opus mono, 16 kHz, 60 ms frames.
 * The kerchunk audio thread runs at 48 kHz, so this module resamples
 * 48k↔16k via kerchunk_resample_into() in both directions.
 *
 * Config section [zello]:
 *   enabled         = yes
 *   server_url      = wss://zello.io/ws         ; F&F default; for Work use
 *                                                ; wss://zellowork.io/ws/<network>
 *   username        = kerchunkd
 *   password        = ...                       ; consumer Zello account password
 *   channel         = mychannel
 *   auth_token      = eyJ...                    ; JWT from developers.zello.com
 *   auth_token_file = /etc/kerchunk/zello.jwt   ; alternative — JWT loaded from file
 *   listen_only     = no
 *   rf_to_zello     = yes                       ; forward RF RX audio to Zello
 *   zello_to_rf     = yes                       ; forward Zello audio to RF TX
 *   priority        = 3                         ; TX queue priority for Zello→RF audio
 *   virtual_user_id = 998                       ; kerchunk user_id stamped on VCOR
 *                                                ; events for inbound Zello audio
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "kerchunk_queue.h"
#include "kerchunk_wav.h"             /* kerchunk_resample_into */
#include <libzello/zello_client.h>
#include <libzello/zello.h>

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOG_MOD "zello"

static kerchunk_core_t *g_core;
static zello_client_t  *g_cli;

/* libzello service thread.
 *
 * Earlier revisions polled libzello from a 10 ms timer on the kerchunk
 * main thread. That starved the main loop: zello_client_poll() blocks
 * inside poll() for up to the requested timeout, and the main thread
 * also drives HID/COR reads at a 20 ms tick. The result was missed COR
 * transitions and audio-queue under-runs on long Zello transmissions.
 *
 * Now the service runs on its own thread so it can sit in poll() as
 * long as it wants without touching the host's tick budget. The audio
 * thread's send_pcm() goes through libzello's lock-free SPSC ring, so
 * it never blocks waiting for this thread; control callbacks fire
 * here, on the service thread. */
static pthread_t        g_zello_thread;
static volatile int     g_zello_thread_running;
static int              g_zello_thread_started;

/* ── Config ────────────────────────────────────────────────────── */

static int   g_enabled;
static char  g_server_url[256];
static char  g_username[64];
static char  g_password[128];
static char  g_channel[128];
static char *g_auth_token;          /* heap-allocated — may be long */
static int   g_listen_only;
static int   g_rf_to_zello;
static int   g_zello_to_rf;
static int   g_priority;
static int   g_virtual_user_id;

/* ── State ─────────────────────────────────────────────────────── */

static int  g_rf_rx_active;          /* RF COR is asserted; we're TX'ing to Zello */
static int  g_zello_rx_active;       /* a remote Zello user is talking */
static char g_zello_rx_username[64]; /* current remote speaker (from libzello on_stream_start) */

/* TX dump: when ZELLO_TX_DUMP_DIR is set in the environment, the post-
 * resample 16 kHz mono PCM samples we feed libzello are appended to a
 * per-stream WAV file there. Lets us audit "is mod_zello pushing the
 * right audio into libzello?" without involving Zello servers. */
static FILE     *g_tx_dump_fp;
static uint32_t  g_tx_dump_samples;
static char      g_tx_dump_path[256];

/* ── Audio rates ───────────────────────────────────────────────── */

#define ZELLO_RATE             16000
#define KERCHUNK_RATE          48000
#define KERCHUNK_FRAME_SAMPLES 960     /* 20 ms @ 48 kHz */
#define ZELLO_FRAME_SAMPLES_20MS 320   /* 20 ms @ 16 kHz — what we feed libzello */

/* ── libzello log bridge ───────────────────────────────────────── */

static void zello_log_bridge(int level, const char *msg, void *ud)
{
    (void)ud;
    int klevel;
    switch (level) {
    case ZELLO_LOG_ERROR: klevel = KERCHUNK_LOG_ERROR; break;
    case ZELLO_LOG_WARN:  klevel = KERCHUNK_LOG_WARN;  break;
    case ZELLO_LOG_INFO:  klevel = KERCHUNK_LOG_INFO;  break;
    case ZELLO_LOG_DEBUG: klevel = KERCHUNK_LOG_DEBUG; break;
    default:              klevel = KERCHUNK_LOG_DEBUG; break;
    }
    g_core->log(klevel, LOG_MOD, "%s", msg);
}

/* ── libzello → kerchunk callbacks ─────────────────────────────── */

static void on_zello_connected(zello_client_t *c, const char *refresh_token, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "channel ready (refresh_token=%s)",
                refresh_token ? "captured" : "none");
}

static void on_zello_disconnected(zello_client_t *c, int code,
                                   const char *reason, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "disconnected (code=%d reason=%s)", code, reason ? reason : "");
    /* libzello schedules its own reconnect; release any kerchunk PTT
     * we were holding so the audio path doesn't get stuck. */
    if (g_zello_rx_active) {
        g_zello_rx_active = 0;
        g_core->release_ptt("zello");
        kerchevt_t vc = { .type = KERCHEVT_VCOR_DROP,
            .vcor = { .source = "zello", .user_id = g_virtual_user_id,
                      .username = g_zello_rx_username[0] ? g_zello_rx_username : NULL } };
        kerchevt_fire(&vc);
        g_zello_rx_username[0] = '\0';
    }
}

static void on_zello_channel_status(zello_client_t *c, const char *chan,
                                     bool online, int users, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "channel '%s' %s (%d users online)",
                chan ? chan : "?", online ? "online" : "offline", users);
}

static void on_zello_stream_start(zello_client_t *c, uint32_t sid,
                                   const char *from, int sr, int frame_ms,
                                   void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "stream %u from=%s sr=%d frame=%dms",
                sid, from ? from : "?", sr, frame_ms);

    if (!g_zello_to_rf) return;

    /* Same shape as mod_poc: hold kerchunk PTT for the whole Zello
     * transmission, then drop. Without this the queue auto-PTT
     * cycles per frame and chops the audio. */
    g_zello_rx_active = 1;
    g_core->request_ptt("zello");

    /* Stash the speaker's Zello username so mod_recorder etc. can
     * attribute the recording to the actual remote operator instead of
     * the bridge's virtual_user_id. Lives until the matching
     * VCOR_DROP fires (or the WS drops mid-stream). */
    if (from && *from)
        snprintf(g_zello_rx_username, sizeof(g_zello_rx_username), "%s", from);
    else
        g_zello_rx_username[0] = '\0';

    /* Virtual COR — feeds mod_recorder / mod_courtesy / mod_asr / mod_cdr
     * the same way RF COR does. user_id stays the configured virtual id
     * (Zello speakers aren't kerchunk users); username carries the
     * remote operator's Zello account name. */
    kerchevt_t vc = { .type = KERCHEVT_VCOR_ASSERT,
        .vcor = { .source = "zello", .user_id = g_virtual_user_id,
                  .username = g_zello_rx_username[0] ? g_zello_rx_username : NULL } };
    kerchevt_fire(&vc);
}

static void on_zello_audio(zello_client_t *c, uint32_t sid,
                            const int16_t *pcm, size_t n_samples,
                            int sample_rate, void *ud)
{
    (void)c; (void)sid; (void)ud;
    if (!g_zello_to_rf || !g_zello_rx_active) return;

    /* Resample Zello rate → 48 kHz. At the default 16 kHz, 60 ms = 960
     * samples in, 2880 out — well within the typical PCM stack alloc. */
    size_t out_max = n_samples * KERCHUNK_RATE / sample_rate + 16;
    int16_t *upsampled = malloc(out_max * sizeof(int16_t));
    if (!upsampled) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "OOM resampling Zello audio");
        return;
    }
    size_t up_n = kerchunk_resample_into(upsampled, out_max,
                                          pcm, n_samples,
                                          sample_rate, KERCHUNK_RATE);
    kerchunk_queue_add_buffer_src(upsampled, (int)up_n, g_priority,
                                   QUEUE_FLAG_NO_TAIL, "zello");
    free(upsampled);
}

static void on_zello_stream_stop(zello_client_t *c, uint32_t sid, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "stream %u stop", sid);
    if (g_zello_rx_active) {
        g_zello_rx_active = 0;
        g_core->release_ptt("zello");
        kerchevt_t vc = { .type = KERCHEVT_VCOR_DROP,
            .vcor = { .source = "zello", .user_id = g_virtual_user_id,
                      .username = g_zello_rx_username[0] ? g_zello_rx_username : NULL } };
        kerchevt_fire(&vc);
        g_zello_rx_username[0] = '\0';
    }
}

static void on_zello_text(zello_client_t *c, const char *from,
                           const char *text, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "text from %s: %s", from ? from : "?", text ? text : "");
}

static void on_zello_error(zello_client_t *c, const char *code, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "server error: %s",
                code ? code : "?");
}

/* ── TX-dump helpers (debug; controlled by ZELLO_TX_DUMP_DIR env) ── */

static void tx_dump_open(void)
{
    const char *dir = getenv("ZELLO_TX_DUMP_DIR");
    if (!dir || !*dir) return;
    time_t   now = time(NULL);
    struct tm tm; localtime_r(&now, &tm);
    snprintf(g_tx_dump_path, sizeof(g_tx_dump_path),
             "%s/zello_tx_%04d%02d%02d_%02d%02d%02d.wav",
             dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    g_tx_dump_fp = fopen(g_tx_dump_path, "wb");
    if (!g_tx_dump_fp) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "tx_dump_open: fopen %s failed", g_tx_dump_path);
        return;
    }
    /* Placeholder RIFF/WAVE header (16 kHz, mono, 16-bit). Sizes get
     * patched in tx_dump_close. */
    uint8_t hdr[44] = {
        'R','I','F','F', 0,0,0,0,
        'W','A','V','E','f','m','t',' ',
        16,0,0,0, 1,0, 1,0,
        0x80,0x3E,0,0,         /* sample_rate    = 16000 */
        0x00,0x7D,0,0,         /* byte_rate      = 32000 */
        2,0, 16,0,
        'd','a','t','a', 0,0,0,0,
    };
    fwrite(hdr, 1, sizeof(hdr), g_tx_dump_fp);
    g_tx_dump_samples = 0;
}

static void tx_dump_write(const int16_t *pcm, size_t n)
{
    if (!g_tx_dump_fp) return;
    fwrite(pcm, sizeof(int16_t), n, g_tx_dump_fp);
    g_tx_dump_samples += (uint32_t)n;
}

static void tx_dump_close(void)
{
    if (!g_tx_dump_fp) return;
    uint32_t data_size = g_tx_dump_samples * 2;
    uint32_t riff_size = 36 + data_size;
    fseek(g_tx_dump_fp, 4, SEEK_SET);
    fwrite(&riff_size, 4, 1, g_tx_dump_fp);
    fseek(g_tx_dump_fp, 40, SEEK_SET);
    fwrite(&data_size, 4, 1, g_tx_dump_fp);
    fclose(g_tx_dump_fp);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "tx_dump_close: wrote %s (%u samples, %.2fs)",
                g_tx_dump_path, g_tx_dump_samples,
                (double)g_tx_dump_samples / ZELLO_RATE);
    g_tx_dump_fp = NULL;
    g_tx_dump_samples = 0;
}

/* ── kerchunk event handlers ───────────────────────────────────── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_rf_to_zello || !g_cli) return;
    if (zello_client_state(g_cli) != ZELLO_STATE_ONLINE) return;

    if (zello_client_start_tx(g_cli) != ZELLO_OK) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "start_tx failed");
        return;
    }
    /* Open the dump file FIRST, then enable the audio-thread path. The
     * audio thread reads g_rf_rx_active before pushing; ordering this
     * way guarantees the first frame it sees has a place to land. */
    tx_dump_open();
    g_rf_rx_active = 1;
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_rf_rx_active || !g_cli) return;
    g_rf_rx_active = 0;
    zello_client_stop_tx(g_cli);
    tx_dump_close();
}

static void on_audio_frame(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_rf_to_zello || !g_rf_rx_active || !g_cli) return;

    /* Resample 48 kHz → 16 kHz (kerchunk audio → libzello encoder rate). */
    int16_t pcm16k[ZELLO_FRAME_SAMPLES_20MS + 16];
    size_t out_n = kerchunk_resample_into(pcm16k, sizeof(pcm16k)/sizeof(pcm16k[0]),
                                           evt->audio.samples, evt->audio.n,
                                           KERCHUNK_RATE, ZELLO_RATE);
    tx_dump_write(pcm16k, out_n);
    zello_client_send_pcm(g_cli, pcm16k, out_n);
}

static void on_shutdown(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_cli) {
        zello_client_stop(g_cli);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "client stopped");
    }
}

static void on_config_reload(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "config reload requested");
    /* Full reload happens via configure(); the core invokes it for us. */
}

/* ── libzello service thread ───────────────────────────────────── */

static void *zello_service_thread(void *arg)
{
    (void)arg;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "service thread started");
    while (g_zello_thread_running) {
        if (g_cli)
            zello_client_poll(g_cli, 100);   /* up to 100 ms in poll() */
        else
            usleep(10000);                   /* config in flight; back off */
    }
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "service thread exiting");
    return NULL;
}

/* ── Helpers ───────────────────────────────────────────────────── */

/* Load a JWT (or any string blob) from a file. Returns malloc'd string
 * with trailing whitespace stripped. */
static char *load_text_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || n > 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    /* Trim trailing whitespace/newlines */
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' ||
                       buf[got - 1] == ' '  || buf[got - 1] == '\t')) {
        buf[--got] = 0;
    }
    return buf;
}

/* ── CLI commands ──────────────────────────────────────────────── */

static const char *state_name(zello_state_t s)
{
    switch (s) {
    case ZELLO_STATE_OFFLINE:    return "offline";
    case ZELLO_STATE_CONNECTING: return "connecting";
    case ZELLO_STATE_LOGON:      return "logon";
    case ZELLO_STATE_ONLINE:     return "online";
    case ZELLO_STATE_RECONNECT:  return "reconnect";
    }
    return "?";
}

static int cli_zello(int argc, const char **argv, kerchunk_resp_t *resp)
{
    resp_init(resp);

    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        const char *st = g_cli ? state_name(zello_client_state(g_cli)) : "unloaded";
        resp_str(resp, "state",         st);
        resp_str(resp, "server",        g_server_url);
        resp_str(resp, "channel",       g_channel);
        resp_str(resp, "username",      g_username);
        resp_bool(resp, "listen_only",  g_listen_only);
        resp_bool(resp, "rf_to_zello",  g_rf_to_zello);
        resp_bool(resp, "zello_to_rf",  g_zello_to_rf);
        resp_bool(resp, "rf_rx_active", g_rf_rx_active);
        resp_bool(resp, "zello_rx_active", g_zello_rx_active);
        char buf[640];
        snprintf(buf, sizeof(buf),
                 "Zello: %s, server=%s, channel=%s, user=%s%s",
                 st, g_server_url, g_channel, g_username,
                 g_listen_only ? " (listen-only)" : "");
        resp_text_raw(resp, buf);
        resp_finish(resp);
        return 0;
    }

    if (strcmp(argv[1], "connect") == 0) {
        if (!g_cli) {
            resp_text_raw(resp, "zello not configured");
            resp_finish(resp);
            return -1;
        }
        int rc = zello_client_start(g_cli);
        char buf[64];
        snprintf(buf, sizeof(buf), "connect: %s", rc == ZELLO_OK ? "ok" : "failed");
        resp_text_raw(resp, buf);
        resp_bool(resp, "ok", rc == ZELLO_OK);
        resp_finish(resp);
        return rc;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        if (!g_cli) {
            resp_text_raw(resp, "zello not configured");
            resp_finish(resp);
            return -1;
        }
        zello_client_stop(g_cli);
        resp_text_raw(resp, "disconnected");
        resp_bool(resp, "ok", 1);
        resp_finish(resp);
        return 0;
    }

    /* `zello wav <path>` — stream a 16-bit mono WAV file straight into
     * the Zello channel, bypassing the RF capture / audio-thread path.
     * Same flow as zello_cli's wav command. Use this to A/B-test
     * whether the bug is in mod_zello's RF→Zello audio (the wav case
     * will sound clean) or in libzello/Zello (the wav case will also
     * sound bad). Only allowed when no RF transmit is currently
     * active, otherwise the two streams would interleave. */
    if (strcmp(argv[1], "wav") == 0) {
        if (argc < 3) {
            resp_text_raw(resp, "Usage: zello wav <path>");
            resp_finish(resp);
            return -1;
        }
        if (!g_cli) {
            resp_text_raw(resp, "zello not configured");
            resp_finish(resp);
            return -1;
        }
        if (zello_client_state(g_cli) != ZELLO_STATE_ONLINE) {
            resp_text_raw(resp, "zello not online");
            resp_finish(resp);
            return -1;
        }
        if (g_rf_rx_active) {
            resp_text_raw(resp, "RF TX in progress — try again after the carrier drops");
            resp_finish(resp);
            return -1;
        }

        int16_t *wav     = NULL;
        size_t   wav_n   = 0;
        int      wav_sr  = 0;
        errno = 0;
        if (kerchunk_wav_read(argv[2], &wav, &wav_n, &wav_sr) != 0) {
            char buf[320];
            /* Preserve errno from the fopen inside kerchunk_wav_read so
             * the operator can tell "no such file" apart from "denied
             * by ProtectHome=true" — the latter is silently confusing
             * because the file exists and the unix bits look ok. */
            snprintf(buf, sizeof(buf),
                     "wav: failed to read %s (%s) — note: kerchunkd "
                     "runs as the kerchunk user with ProtectHome=true, "
                     "so /home is unreadable; stage the file in "
                     "/var/lib/kerchunk or use /usr/share/kerchunk/sounds",
                     argv[2], errno ? strerror(errno) : "bad WAV format");
            resp_text_raw(resp, buf);
            resp_finish(resp);
            return -1;
        }

        /* Resample to Zello's 16 kHz if the file isn't already there. */
        int16_t *pcm16k = wav;
        size_t   pcm_n  = wav_n;
        int16_t *resampled = NULL;
        if (wav_sr != ZELLO_RATE) {
            size_t cap = wav_n * ZELLO_RATE / wav_sr + 16;
            resampled = malloc(cap * sizeof(int16_t));
            if (!resampled) {
                free(wav);
                resp_text_raw(resp, "wav: OOM resampling");
                resp_finish(resp);
                return -1;
            }
            pcm_n = kerchunk_resample_into(resampled, cap, wav, wav_n,
                                            wav_sr, ZELLO_RATE);
            pcm16k = resampled;
            free(wav);
            wav = NULL;
        }

        if (zello_client_start_tx(g_cli) != ZELLO_OK) {
            free(resampled); free(wav);
            resp_text_raw(resp, "wav: start_tx failed");
            resp_finish(resp);
            return -1;
        }

        /* Push the whole file in 60 ms (960-sample) chunks — mirrors
         * the zello_cli wav path exactly so any divergence here vs
         * there narrows the bug down. libzello's SPSC ring is 2 s; a
         * file longer than that would need pacing, but for verifying
         * the codec/transport this is fine. */
        const size_t CHUNK = ZELLO_RATE * 60 / 1000;   /* 960 */
        size_t sent = 0;
        while (sent < pcm_n) {
            size_t take = (pcm_n - sent) < CHUNK ? (pcm_n - sent) : CHUNK;
            if (zello_client_send_pcm(g_cli, pcm16k + sent, take) != ZELLO_OK) {
                g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                            "wav: send_pcm failed at sample %zu/%zu", sent, pcm_n);
                break;
            }
            sent += take;
        }

        /* Let the service thread drain at the encoder's nominal rate
         * before sending stop_stream — the wav playback length plus a
         * small tail for the last partial frame and protocol round-
         * trips. Sleeping here is fine: cli_zello runs on the CLI
         * thread, not on any real-time audio thread. */
        long playback_ms = (long)((double)pcm_n / ZELLO_RATE * 1000.0);
        usleep((useconds_t)(playback_ms + 300) * 1000);

        zello_client_stop_tx(g_cli);
        usleep(200000);

        free(resampled);

        char buf[160];
        snprintf(buf, sizeof(buf), "wav: streamed %zu samples (%.2fs) from %s",
                 sent, (double)sent / ZELLO_RATE, argv[2]);
        resp_text_raw(resp, buf);
        resp_int(resp, "samples", (int)sent);
        resp_bool(resp, "ok", 1);
        resp_finish(resp);
        return 0;
    }

    if (strcmp(argv[1], "say") == 0) {
        if (argc < 3) {
            resp_text_raw(resp, "Usage: zello say <text>");
            resp_finish(resp);
            return -1;
        }
        if (!g_cli) {
            resp_text_raw(resp, "zello not configured");
            resp_finish(resp);
            return -1;
        }
        char msg[512] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
        }
        int rc = zello_client_send_text(g_cli, msg);
        char buf[64];
        snprintf(buf, sizeof(buf), "say: %s", rc == ZELLO_OK ? "sent" : "failed");
        resp_text_raw(resp, buf);
        resp_bool(resp, "ok", rc == ZELLO_OK);
        resp_finish(resp);
        return rc;
    }

    resp_text_raw(resp, "Usage: zello [status|connect|disconnect|say <text>|wav <path>]");
    resp_finish(resp);
    return -1;
}

static const kerchunk_ui_field_t say_fields[] = {
    { "text", "Message", "text", NULL, "Hello from kerchunk" },
};

static const kerchunk_ui_field_t wav_fields[] = {
    { "path", "WAV path", "text", NULL, "/usr/share/kerchunk/sounds/system/system_ready.wav" },
};

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "zello", "zello [status|connect|disconnect|say|wav]",
      "Zello channel bridge management", cli_zello,
      .category = "Zello", .ui_label = "Status",
      .ui_type = CLI_UI_BUTTON, .ui_command = "zello status",
      .subcommands = "status,connect,disconnect,say,wav" },
    { "zello connect", "zello connect",
      "Connect to the Zello channel", cli_zello,
      .category = "Zello", .ui_label = "Connect",
      .ui_type = CLI_UI_BUTTON, .ui_command = "zello connect" },
    { "zello disconnect", "zello disconnect",
      "Disconnect from the Zello channel", cli_zello,
      .category = "Zello", .ui_label = "Disconnect",
      .ui_type = CLI_UI_BUTTON, .ui_command = "zello disconnect" },
    { "zello say", "zello say <text>",
      "Send a text message to the channel", cli_zello,
      .category = "Zello", .ui_label = "Say",
      .ui_type = CLI_UI_FORM, .ui_command = "zello say",
      .ui_fields = say_fields, .num_ui_fields = 1 },
    { "zello wav", "zello wav <path>",
      "Stream a 16-bit mono WAV file to the Zello channel (test tool)",
      cli_zello,
      .category = "Zello", .ui_label = "Play WAV",
      .ui_type = CLI_UI_FORM, .ui_command = "zello wav",
      .ui_fields = wav_fields, .num_ui_fields = 1 },
};

/* ── Module lifecycle ──────────────────────────────────────────── */

/* Tear down the live client + timer. Does NOT free g_auth_token —
 * that's a config-owned value freed/reloaded at the top of
 * mod_configure() (so callers can safely invoke teardown after
 * loading a new token). */
static void teardown_client(void)
{
    /* Stop the service thread BEFORE destroying the client so the
     * thread can't fire one last zello_client_poll() against a freed
     * handle. zello_client_stop() closes the WS, which causes any
     * blocking poll() inside the service thread to return promptly. */
    if (g_zello_thread_started) {
        g_zello_thread_running = 0;
        if (g_cli) zello_client_stop(g_cli);
        pthread_join(g_zello_thread, NULL);
        g_zello_thread_started = 0;
    }
    if (g_cli) {
        zello_client_destroy(g_cli);
        g_cli = NULL;
    }
    g_rf_rx_active = 0;
    g_zello_rx_active = 0;
}

static int mod_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_COR_ASSERT,    on_cor_assert,    NULL);
    core->subscribe(KERCHEVT_COR_DROP,      on_cor_drop,      NULL);
    core->subscribe(KERCHEVT_AUDIO_FRAME,   on_audio_frame,   NULL);
    core->subscribe(KERCHEVT_SHUTDOWN,      on_shutdown,      NULL);
    core->subscribe(KERCHEVT_CONFIG_RELOAD, on_config_reload, NULL);
    return 0;
}

static int mod_configure(const kerchunk_config_t *cfg)
{
    (void)cfg;

    g_enabled = g_core->config_get_int("zello", "enabled", 0);
    if (!g_enabled) {
        teardown_client();
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled by config");
        return 0;
    }

    const char *server_url = g_core->config_get("zello", "server_url");
    snprintf(g_server_url, sizeof(g_server_url), "%s",
             server_url ? server_url : "wss://zello.io/ws");

    const char *username = g_core->config_get("zello", "username");
    const char *password = g_core->config_get("zello", "password");
    const char *channel  = g_core->config_get("zello", "channel");
    if (!username || !channel) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "missing required config: username and channel");
        return -1;
    }
    snprintf(g_username, sizeof(g_username), "%s", username);
    snprintf(g_password, sizeof(g_password), "%s", password ? password : "");
    snprintf(g_channel,  sizeof(g_channel),  "%s", channel);

    g_listen_only     = g_core->config_get_int("zello", "listen_only", 0);
    g_rf_to_zello     = g_core->config_get_int("zello", "rf_to_zello", 1);
    g_zello_to_rf     = g_core->config_get_int("zello", "zello_to_rf", 1);
    g_priority        = g_core->config_get_int("zello", "priority", KERCHUNK_PRI_NORMAL);
    g_virtual_user_id = g_core->config_get_int("zello", "virtual_user_id", 998);

    /* JWT: prefer file (more practical for 500B+ tokens). */
    if (g_auth_token) { free(g_auth_token); g_auth_token = NULL; }
    const char *tok_file = g_core->config_get("zello", "auth_token_file");
    const char *tok_inline = g_core->config_get("zello", "auth_token");
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "auth_token_file='%s' auth_token_inline=%s",
                tok_file ? tok_file : "(null)",
                tok_inline && *tok_inline ? "(set)" : "(empty)");
    if (tok_file && *tok_file) {
        g_auth_token = load_text_file(tok_file);
        if (!g_auth_token) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "auth_token_file '%s' unreadable — Zello F&F will reject logon", tok_file);
        } else {
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "auth_token loaded from %s (%zu bytes)",
                        tok_file, strlen(g_auth_token));
        }
    } else if (tok_inline && *tok_inline) {
        g_auth_token = strdup(tok_inline);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "auth_token loaded inline (%zu bytes)", strlen(g_auth_token));
    } else {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "no auth_token configured — Zello F&F logon will fail");
    }

    /* Tear down existing client on reconfigure. */
    teardown_client();

    /* Wire libzello logging into the kerchunk logger. */
    zello_set_log_callback(zello_log_bridge, NULL);
    zello_set_log_level(ZELLO_LOG_INFO);

    zello_config_t zcfg = {
        .server_url  = g_server_url,
        .username    = g_username,
        .password    = g_password[0] ? g_password : NULL,
        .channel     = g_channel,
        .auth_token  = g_auth_token,
        .listen_only = g_listen_only,
    };
    zello_callbacks_t cb = {
        .on_connected      = on_zello_connected,
        .on_disconnected   = on_zello_disconnected,
        .on_channel_status = on_zello_channel_status,
        .on_stream_start   = on_zello_stream_start,
        .on_audio          = on_zello_audio,
        .on_stream_stop    = on_zello_stream_stop,
        .on_text_message   = on_zello_text,
        .on_error          = on_zello_error,
    };

    g_cli = zello_client_create(&zcfg, &cb);
    if (!g_cli) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "zello_client_create failed (check username/channel)");
        return -1;
    }

    int rc = zello_client_start(g_cli);
    if (rc != ZELLO_OK) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "zello_client_start returned %d — will retry via reconnect", rc);
    }

    /* Dedicated service thread, NOT a kerchunk timer — see comment
     * next to g_zello_thread for why. */
    g_zello_thread_running = 1;
    if (pthread_create(&g_zello_thread, NULL, zello_service_thread, NULL) != 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "pthread_create failed");
        g_zello_thread_running = 0;
        teardown_client();
        return -1;
    }
    g_zello_thread_started = 1;

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "client started: server=%s channel=%s user=%s%s",
                g_server_url, g_channel, g_username,
                g_listen_only ? " (listen-only)" : "");
    return 0;
}

static void mod_unload(void)
{
    teardown_client();
    if (g_auth_token) { free(g_auth_token); g_auth_token = NULL; }

    g_core->unsubscribe(KERCHEVT_COR_ASSERT,    on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,      on_cor_drop);
    g_core->unsubscribe(KERCHEVT_AUDIO_FRAME,   on_audio_frame);
    g_core->unsubscribe(KERCHEVT_SHUTDOWN,      on_shutdown);
    g_core->unsubscribe(KERCHEVT_CONFIG_RELOAD, on_config_reload);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded");
}

static const kerchunk_module_def_t mod_def = {
    .name             = "mod_zello",
    .version          = "0.1.0",
    .description      = "Zello Channel API bridge",
    .load             = mod_load,
    .configure        = mod_configure,
    .unload           = mod_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_def);
