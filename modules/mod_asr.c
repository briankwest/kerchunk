/*
 * mod_asr.c — Automatic speech recognition for inbound transmissions
 *
 * Captures RX audio via the audio tap during COR active, then sends
 * the complete audio to a Wyoming ASR server (e.g. wyoming-faster-whisper)
 * after COR drops.  The transcript is logged, fired as an event, and
 * available via CLI/dashboard.
 *
 * Config: [asr] section in kerchunk.conf
 * Requires: libwyoming
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"

#ifdef HAVE_LIBWYOMING
#include <libwyoming/wyoming.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

#define LOG_MOD "asr"
#define MAX_CAPTURE_S     60   /* hard cap: 60 seconds */
#define MAX_TRANSCRIPTS   50   /* rolling buffer of recent transcripts */

static kerchunk_core_t *g_core;

/* Config */
static int  g_enabled;
static char g_host[64]     = "127.0.0.1";
static int  g_port         = 10300;
static char g_language[16] = "en";
static int  g_max_capture_s = 30;
static int  g_min_duration_ms = 500;  /* skip very short transmissions */

/* Capture state */
static atomic_int g_capturing;
static int16_t   *g_cap_buf;
static size_t     g_cap_len;
static size_t     g_cap_cap;
static int        g_caller_id;

/* Transcript history */
typedef struct {
    char     text[512];
    char     caller[32];
    time_t   timestamp;
    float    duration_s;
} transcript_t;

static transcript_t g_transcripts[MAX_TRANSCRIPTS];
static int           g_transcript_idx;
static int           g_transcript_count;
static pthread_mutex_t g_transcript_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Background worker */
typedef struct {
    int16_t *pcm;
    size_t   samples;
    int      rate;
    int      caller_id;
} asr_request_t;

static asr_request_t  g_asr_req;
static int             g_asr_pending;
static pthread_mutex_t g_asr_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_asr_cond  = PTHREAD_COND_INITIALIZER;
static int             g_asr_tid   = -1;

/* ── Audio tap callback ── */

static void rx_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_capturing || !evt->audio.samples)
        return;

    size_t max_samples = (size_t)g_core->sample_rate * (size_t)g_max_capture_s;
    if (g_cap_len >= max_samples)
        return;

    size_t n = evt->audio.n;
    size_t needed = g_cap_len + n;
    if (needed > g_cap_cap) {
        size_t new_cap = g_cap_cap * 2;
        if (new_cap < needed) new_cap = needed + (size_t)g_core->sample_rate;
        if (new_cap > max_samples) new_cap = max_samples;
        int16_t *new_buf = realloc(g_cap_buf, new_cap * sizeof(int16_t));
        if (!new_buf) return;
        g_cap_buf = new_buf;
        g_cap_cap = new_cap;
    }

    size_t to_copy = n;
    if (g_cap_len + to_copy > max_samples)
        to_copy = max_samples - g_cap_len;
    memcpy(g_cap_buf + g_cap_len, evt->audio.samples, to_copy * sizeof(int16_t));
    g_cap_len += to_copy;
}

/* ── Store transcript ── */

static void store_transcript(const char *text, const char *caller, float dur)
{
    pthread_mutex_lock(&g_transcript_mutex);
    transcript_t *t = &g_transcripts[g_transcript_idx];
    snprintf(t->text, sizeof(t->text), "%s", text);
    snprintf(t->caller, sizeof(t->caller), "%s", caller ? caller : "unknown");
    t->timestamp = time(NULL);
    t->duration_s = dur;
    g_transcript_idx = (g_transcript_idx + 1) % MAX_TRANSCRIPTS;
    if (g_transcript_count < MAX_TRANSCRIPTS)
        g_transcript_count++;
    pthread_mutex_unlock(&g_transcript_mutex);
}

/* ── Worker thread ── */

static void *asr_worker(void *arg)
{
    (void)arg;

    while (!g_core->thread_should_stop(g_asr_tid)) {
        pthread_mutex_lock(&g_asr_mutex);
        while (!g_asr_pending && !g_core->thread_should_stop(g_asr_tid)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_asr_cond, &g_asr_mutex, &ts);
        }
        if (g_core->thread_should_stop(g_asr_tid)) {
            pthread_mutex_unlock(&g_asr_mutex);
            break;
        }

        /* Take ownership of the request */
        int16_t *pcm       = g_asr_req.pcm;
        size_t   samples   = g_asr_req.samples;
        int      rate      = g_asr_req.rate;
        int      caller_id = g_asr_req.caller_id;
        g_asr_req.pcm = NULL;
        g_asr_pending = 0;
        pthread_mutex_unlock(&g_asr_mutex);

        if (!pcm || samples == 0)
            continue;

        float dur = (float)samples / (float)rate;

        /* Look up caller name */
        const char *caller_name = "unknown";
        if (caller_id > 0) {
            const kerchunk_user_t *u = g_core->user_lookup_by_id(caller_id);
            if (u) caller_name = u->name;
        }

#ifdef HAVE_LIBWYOMING
        /* Connect to ASR server */
        wyoming_conn_t *conn = wyoming_connect(g_host, (uint16_t)g_port);
        if (!conn) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "ASR connect failed: %s:%d", g_host, g_port);
            free(pcm);
            continue;
        }

        wyoming_audio_format_t fmt = { .rate = rate, .width = 2, .channels = 1 };
        char *text = NULL;
        wyoming_error_t rc = wyoming_transcribe_pcm(conn, pcm, samples,
                                                     &fmt, g_language, &text);
        wyoming_close(conn);
#else
        (void)rate;
        wyoming_error_t rc = WYOMING_ERR_PROTO;
        char *text = NULL;
#endif
        free(pcm);

        if (rc == WYOMING_OK && text && text[0]) {
            /* Strip leading/trailing whitespace */
            char *s = text;
            while (*s == ' ') s++;
            size_t len = strlen(s);
            while (len > 0 && s[len-1] == ' ') s[--len] = '\0';

            if (s[0]) {
                g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                            "[%s %.1fs] %s", caller_name, dur, s);

                store_transcript(s, caller_name, dur);

                /* Fire event so dashboard/webhook can display it */
                kerchevt_t ae = {
                    .type = KERCHEVT_ANNOUNCEMENT,
                    .announcement = {
                        .source = "asr",
                        .description = s,
                    },
                };
                kerchevt_fire(&ae);
            }
        } else if (rc != WYOMING_OK) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "transcription failed (%d) for %.1fs from %s",
                        rc, dur, caller_name);
        }

        free(text);
    }

    return NULL;
}

/* ── Event handlers ── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;

    g_capturing = 1;
    g_cap_len = 0;
    g_cap_cap = (size_t)g_core->sample_rate * 10;  /* start with 10s buffer */
    g_cap_buf = malloc(g_cap_cap * sizeof(int16_t));
    if (!g_cap_buf) { g_capturing = 0; return; }
    g_caller_id = 0;
    g_core->audio_tap_register(rx_audio_tap, NULL);
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_capturing) return;
    g_capturing = 0;
    g_core->audio_tap_unregister(rx_audio_tap);

    if (!g_cap_buf || g_cap_len == 0) {
        free(g_cap_buf);
        g_cap_buf = NULL;
        return;
    }

    /* Skip very short transmissions (kerchunks, noise) */
    float dur_ms = (float)g_cap_len / (float)g_core->sample_rate * 1000.0f;
    if (dur_ms < (float)g_min_duration_ms) {
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                    "skipping short TX (%.0fms < %dms)", dur_ms, g_min_duration_ms);
        free(g_cap_buf);
        g_cap_buf = NULL;
        return;
    }

    /* Hand off to worker thread */
    pthread_mutex_lock(&g_asr_mutex);

    /* Drop previous pending request if worker is still busy */
    if (g_asr_pending && g_asr_req.pcm) {
        free(g_asr_req.pcm);
    }

    g_asr_req.pcm       = g_cap_buf;
    g_asr_req.samples    = g_cap_len;
    g_asr_req.rate       = g_core->sample_rate;
    g_asr_req.caller_id  = g_caller_id;
    g_asr_pending = 1;
    g_cap_buf = NULL;
    g_cap_len = 0;

    pthread_cond_signal(&g_asr_cond);
    pthread_mutex_unlock(&g_asr_mutex);
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_caller_id = evt->caller.user_id;
}

/* ── Module lifecycle ── */

static int asr_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_COR_ASSERT, on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP, on_cor_drop, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    return 0;
}

static int asr_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "asr", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "asr", "wyoming_host");
    if (v) snprintf(g_host, sizeof(g_host), "%s", v);

    v = kerchunk_config_get(cfg, "asr", "wyoming_port");
    if (v) g_port = atoi(v);

    v = kerchunk_config_get(cfg, "asr", "language");
    if (v) snprintf(g_language, sizeof(g_language), "%s", v);

    g_max_capture_s = kerchunk_config_get_int(cfg, "asr", "max_capture", 30);
    if (g_max_capture_s > MAX_CAPTURE_S) g_max_capture_s = MAX_CAPTURE_S;
    if (g_max_capture_s < 1) g_max_capture_s = 1;

    g_min_duration_ms = kerchunk_config_get_duration_ms(cfg, "asr", "min_duration", 500);
    if (g_min_duration_ms < 0) g_min_duration_ms = 0;

    if (!g_enabled) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled");
        return 0;
    }

#ifndef HAVE_LIBWYOMING
    g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                "ASR requires libwyoming — module disabled");
    g_enabled = 0;
    return 0;
#endif

    /* Stop existing worker on reload */
    if (g_asr_tid >= 0) {
        g_core->thread_stop(g_asr_tid);
        pthread_cond_signal(&g_asr_cond);
        g_core->thread_join(g_asr_tid);
        g_asr_tid = -1;
    }

    g_asr_pending = 0;
    g_asr_tid = g_core->thread_create("asr-worker", asr_worker, NULL);
    if (g_asr_tid < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create worker thread");
        g_enabled = 0;
        return -1;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "ready: %s:%d lang=%s max=%ds min=%dms",
                g_host, g_port, g_language, g_max_capture_s, g_min_duration_ms);
    return 0;
}

static void asr_unload(void)
{
    g_enabled = 0;
    g_capturing = 0;

    if (g_asr_tid >= 0) {
        g_core->thread_stop(g_asr_tid);
        pthread_cond_signal(&g_asr_cond);
        g_core->thread_join(g_asr_tid);
        g_asr_tid = -1;
    }

    free(g_cap_buf);
    g_cap_buf = NULL;

    pthread_mutex_lock(&g_asr_mutex);
    free(g_asr_req.pcm);
    g_asr_req.pcm = NULL;
    pthread_mutex_unlock(&g_asr_mutex);

    g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP, on_cor_drop);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
}

/* ── CLI ── */

static int cli_asr(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    if (argc >= 2 && strcmp(argv[1], "history") == 0) {
        pthread_mutex_lock(&g_transcript_mutex);
        resp_int(r, "count", g_transcript_count);

        char buf[768];
        int show = g_transcript_count < 10 ? g_transcript_count : 10;
        resp_text_raw(r, "Recent transcripts:\n");
        for (int i = 0; i < show; i++) {
            int idx = (g_transcript_idx - 1 - i + MAX_TRANSCRIPTS) % MAX_TRANSCRIPTS;
            transcript_t *t = &g_transcripts[idx];
            struct tm tbuf;
            struct tm *tm = localtime_r(&t->timestamp, &tbuf);
            snprintf(buf, sizeof(buf), "  %02d:%02d:%02d [%s %.1fs] %s\n",
                     tm->tm_hour, tm->tm_min, tm->tm_sec,
                     t->caller, t->duration_s, t->text);
            resp_text_raw(r, buf);
        }
        pthread_mutex_unlock(&g_transcript_mutex);
        return 0;
    }

    /* Default: status */
    resp_bool(r, "enabled", g_enabled);
    resp_str(r, "wyoming_host", g_host);
    resp_int(r, "wyoming_port", g_port);
    resp_str(r, "language", g_language);
    resp_int(r, "max_capture_s", g_max_capture_s);
    resp_int(r, "min_duration_ms", g_min_duration_ms);
    resp_bool(r, "capturing", g_capturing);
    resp_int(r, "transcript_count", g_transcript_count);

    if (g_transcript_count > 0) {
        pthread_mutex_lock(&g_transcript_mutex);
        int last = (g_transcript_idx - 1 + MAX_TRANSCRIPTS) % MAX_TRANSCRIPTS;
        resp_str(r, "last_transcript", g_transcripts[last].text);
        resp_str(r, "last_caller", g_transcripts[last].caller);
        pthread_mutex_unlock(&g_transcript_mutex);
    }
    return 0;

usage:
    resp_text_raw(r, "Automatic speech recognition for inbound transmissions\n\n"
        "  asr\n"
        "    Show ASR status and last transcript.\n\n"
        "  asr history\n"
        "    Show recent transcripts (up to 10).\n\n"
        "Config: [asr] enabled, wyoming_host, wyoming_port,\n"
        "        language, max_capture, min_duration\n");
    resp_str(r, "error", "usage: asr [history]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "asr", .usage = "asr [history]",
      .description = "Speech recognition status and transcripts",
      .handler = cli_asr,
      .category = "Audio", .ui_label = "ASR Status",
      .ui_type = CLI_UI_BUTTON, .ui_command = "asr" },
};

static kerchunk_module_def_t mod_asr = {
    .name             = "mod_asr",
    .version          = "1.0.0",
    .description      = "Automatic speech recognition (Wyoming ASR)",
    .load             = asr_load,
    .configure        = asr_configure,
    .unload           = asr_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_asr);
