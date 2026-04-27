/*
 * mod_asr.c — Automatic speech recognition for inbound transmissions
 *
 * Captures RX audio via the audio tap during COR active and transcribes
 * it via a Wyoming ASR server.
 *
 * Two modes:
 *   batch     — Accumulates audio, sends after COR drop (Whisper)
 *   streaming — Connects on COR assert, feeds chunks in real-time,
 *               transcript available instantly on COR drop (Zipformer)
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
#include "kerchunk_queue.h"

#ifdef HAVE_LIBWYOMING
#include <libwyoming/wyoming.h>
#endif

#ifdef HAVE_NEMO_NORMALIZE
#include "nemo_normalize.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

#define LOG_MOD "asr"
/* Hard cap on per-transmission capture. 300s aligns with the FCC
 * Part 95 GMRS 5-minute transmission limit; longer than that and
 * something else is wrong (stuck PTT, runaway equipment) and the
 * transcript would be useless anyway. Three buffers of this size
 * (cap, stream, tx) at 48kHz × 2B = ~86MB, fine on any Pi class. */
#define MAX_CAPTURE_S     300
#define MAX_TRANSCRIPTS   50   /* rolling buffer of recent transcripts */

static kerchunk_core_t *g_core;

/* Config */
static int  g_enabled;
static char g_host[64]     = "127.0.0.1";
static int  g_port         = 10300;
static char g_language[16] = "en";
static int  g_max_capture_s = 30;
static int  g_min_duration_ms = 500;

typedef enum { ASR_MODE_BATCH, ASR_MODE_STREAMING } asr_mode_t;
static asr_mode_t g_mode = ASR_MODE_BATCH;

#ifdef HAVE_NEMO_NORMALIZE
static void *g_itn = NULL;       /* nemo ITN normalizer (spoken→written) */
static char  g_far_dir[256] = "";
#endif

/* ── Batch mode state (RF inbound via COR) ── */
static atomic_int g_capturing;
static int16_t   *g_cap_buf;
static size_t     g_cap_len;
static size_t     g_cap_cap;
static int        g_caller_id;

/* ── TX capture state (phone/PoC audio via playback tap) ── */
static atomic_int g_tx_capturing;
static int16_t   *g_tx_buf;
static size_t     g_tx_len;
static size_t     g_tx_cap;

/* ── Streaming mode state ── */
#ifdef HAVE_LIBWYOMING
static wyoming_conn_t       *g_stream_conn;
static wyoming_audio_format_t g_stream_fmt;
static size_t                 g_stream_samples;   /* total samples sent */
static uint64_t               g_stream_start_us;
#endif
/* g_streaming_active is read in non-libwyoming code paths
 * (status reporting), so it stays unconditional. */
static atomic_int             g_streaming_active;

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

/* Batch worker */
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

/* ── Shared helpers ── */

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

static const char *get_caller_name(int caller_id)
{
    if (caller_id > 0) {
        const kerchunk_user_t *u = g_core->user_lookup_by_id(caller_id);
        if (u) return u->name;
    }
    return "unknown";
}

static void emit_transcript(const char *raw, const char *caller, float dur)
{
    /* Strip leading/trailing whitespace */
    const char *s = raw;
    while (*s == ' ') s++;
    size_t len = strlen(s);
    char *trimmed = strdup(s);
    if (!trimmed) return;
    while (len > 0 && trimmed[len-1] == ' ') trimmed[--len] = '\0';

#ifdef HAVE_NEMO_NORMALIZE
    /* Inverse text normalization: spoken form → written form
     * e.g. "seventy two" → "72", "ten miles per hour" → "10 mph" */
    if (g_itn && trimmed[0]) {
        char normalized[1024];
        if (nemo_normalize(g_itn, trimmed, normalized, (int)sizeof(normalized)) == 0
            && normalized[0]) {
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                        "ITN: \"%s\" -> \"%s\"", trimmed, normalized);
            free(trimmed);
            trimmed = strdup(normalized);
            if (!trimmed) return;
        }
    }
#endif

    if (trimmed[0]) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "[%s %.1fs] %s", caller, dur, trimmed);

        store_transcript(trimmed, caller, dur);

        kerchevt_t ae = {
            .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "asr", .description = trimmed },
        };
        kerchevt_fire(&ae);
    }
    free(trimmed);
}

/* ── Batch mode: audio tap accumulates, worker sends after COR drop ── */

static void batch_audio_tap(const kerchevt_t *evt, void *ud)
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

static void *batch_worker(void *arg)
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
        const char *caller_name = get_caller_name(caller_id);

#ifdef HAVE_LIBWYOMING
        wyoming_conn_t *conn = wyoming_connect(g_host, (uint16_t)g_port);
        if (!conn) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "batch connect failed: %s:%d", g_host, g_port);
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
        int rc = -1;
        char *text = NULL;
#endif
        free(pcm);

        if (rc == 0 && text && text[0])
            emit_transcript(text, caller_name, dur);
        else if (rc != 0)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "batch transcription failed (%d) for %.1fs from %s",
                        rc, dur, caller_name);
        free(text);
    }
    return NULL;
}

/* ── Streaming mode: connect on COR, feed chunks in real-time ──
 * Only compiled when libwyoming is present; the on_cor_assert /
 * on_cor_drop handlers fall back to batch mode otherwise so the
 * stream tap is genuinely unreachable at that point. */
#ifdef HAVE_LIBWYOMING
static void stream_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_streaming_active || !evt->audio.samples || !g_stream_conn)
        return;

    size_t max_samples = (size_t)g_core->sample_rate * (size_t)g_max_capture_s;
    if (g_stream_samples >= max_samples)
        return;

    size_t n = evt->audio.n;
    if (g_stream_samples + n > max_samples)
        n = max_samples - g_stream_samples;

    wyoming_error_t rc = wyoming_transcribe_chunk(g_stream_conn,
                                                   evt->audio.samples, n,
                                                   &g_stream_fmt);
    if (rc == WYOMING_OK)
        g_stream_samples += n;
    else {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "streaming chunk failed (%d)", rc);
        /* Connection broken — stop streaming, will be cleaned up on COR drop */
        g_streaming_active = 0;
    }
}
#endif

/* ── Event handlers ── */

/* ── TX audio capture (phone/PoC → ASR) ── */

static void tx_playback_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_tx_capturing || !evt->audio.samples)
        return;

    size_t max_samples = (size_t)g_core->sample_rate * (size_t)g_max_capture_s;
    if (g_tx_len >= max_samples)
        return;

    size_t n = evt->audio.n;
    size_t needed = g_tx_len + n;
    if (needed > g_tx_cap) {
        size_t new_cap = g_tx_cap * 2;
        if (new_cap < needed) new_cap = needed + (size_t)g_core->sample_rate;
        if (new_cap > max_samples) new_cap = max_samples;
        int16_t *new_buf = realloc(g_tx_buf, new_cap * sizeof(int16_t));
        if (!new_buf) return;
        g_tx_buf = new_buf;
        g_tx_cap = new_cap;
    }

    size_t to_copy = n;
    if (g_tx_len + to_copy > max_samples)
        to_copy = max_samples - g_tx_len;
    memcpy(g_tx_buf + g_tx_len, evt->audio.samples, to_copy * sizeof(int16_t));
    g_tx_len += to_copy;
}

/* Virtual COR — web PTT, PoC, phone.  Captures TX audio via playback tap
 * between VCOR_ASSERT and VCOR_DROP, then sends to ASR. */
static int g_vcor_user_id;

static void on_vcor_assert(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_enabled) return;
    if (g_tx_capturing) return;

    g_tx_capturing = 1;
    g_tx_len = 0;
    g_vcor_user_id = evt->vcor.user_id;
    g_tx_cap = (size_t)g_core->sample_rate * (size_t)g_max_capture_s;
    g_tx_buf = malloc(g_tx_cap * sizeof(int16_t));
    if (!g_tx_buf) { g_tx_capturing = 0; return; }
    g_core->playback_tap_register(tx_playback_tap, NULL);

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                "VCOR assert: capturing TX audio (source=%s user=%d)",
                evt->vcor.source ? evt->vcor.source : "?", evt->vcor.user_id);
}

static void on_vcor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_tx_capturing) return;
    g_tx_capturing = 0;
    g_core->playback_tap_unregister(tx_playback_tap);

    if (!g_tx_buf || g_tx_len == 0) {
        free(g_tx_buf);
        g_tx_buf = NULL;
        return;
    }

    float dur_ms = (float)g_tx_len / (float)g_core->sample_rate * 1000.0f;
    if (dur_ms < (float)g_min_duration_ms) {
        free(g_tx_buf);
        g_tx_buf = NULL;
        return;
    }

    /* Hand off to batch worker for transcription */
    pthread_mutex_lock(&g_asr_mutex);
    if (g_asr_pending && g_asr_req.pcm)
        free(g_asr_req.pcm);
    g_asr_req.pcm       = g_tx_buf;
    g_asr_req.samples    = g_tx_len;
    g_asr_req.rate       = g_core->sample_rate;
    g_asr_req.caller_id  = g_vcor_user_id;
    g_asr_pending = 1;
    g_tx_buf = NULL;
    g_tx_len = 0;
    pthread_cond_signal(&g_asr_cond);
    pthread_mutex_unlock(&g_asr_mutex);

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                "VCOR drop: TX audio captured for ASR (%.1fs)", dur_ms / 1000.0f);
}

/* ── Event handlers ── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    (void)evt;  /* only used by streaming branch (HAVE_LIBWYOMING) */
    if (!g_enabled) return;

    g_caller_id = 0;

    if (g_mode == ASR_MODE_STREAMING) {
#ifdef HAVE_LIBWYOMING
        /* Connect and start streaming immediately */
        g_stream_conn = wyoming_connect(g_host, (uint16_t)g_port);
        if (!g_stream_conn) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "streaming connect failed: %s:%d", g_host, g_port);
            return;
        }

        g_stream_fmt.rate     = g_core->sample_rate;
        g_stream_fmt.width    = 2;
        g_stream_fmt.channels = 1;
        g_stream_samples = 0;
        g_stream_start_us = evt->timestamp_us;

        wyoming_error_t rc = wyoming_transcribe_start(g_stream_conn,
                                                       &g_stream_fmt,
                                                       g_language);
        if (rc != WYOMING_OK) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "streaming start failed (%d)", rc);
            wyoming_close(g_stream_conn);
            g_stream_conn = NULL;
            return;
        }

        g_streaming_active = 1;
        g_core->audio_tap_register(stream_audio_tap, NULL);
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "streaming started");
#endif
    } else {
        /* Batch mode: accumulate audio */
        g_capturing = 1;
        g_cap_len = 0;
        g_cap_cap = (size_t)g_core->sample_rate * 10;
        g_cap_buf = malloc(g_cap_cap * sizeof(int16_t));
        if (!g_cap_buf) { g_capturing = 0; return; }
        g_core->audio_tap_register(batch_audio_tap, NULL);
    }
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)ud;

    if (g_mode == ASR_MODE_STREAMING) {
#ifdef HAVE_LIBWYOMING
        if (!g_streaming_active && !g_stream_conn) return;
        g_streaming_active = 0;
        g_core->audio_tap_unregister(stream_audio_tap);

        if (!g_stream_conn || g_stream_samples == 0) {
            if (g_stream_conn) {
                wyoming_close(g_stream_conn);
                g_stream_conn = NULL;
            }
            return;
        }

        /* Skip short transmissions */
        float dur_ms = (float)g_stream_samples / (float)g_core->sample_rate * 1000.0f;
        if (dur_ms < (float)g_min_duration_ms) {
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                        "skipping short TX (%.0fms < %dms)", dur_ms, g_min_duration_ms);
            wyoming_close(g_stream_conn);
            g_stream_conn = NULL;
            return;
        }

        /* Stop streaming and get final transcript */
        char *text = NULL;
        wyoming_error_t rc = wyoming_transcribe_stop(g_stream_conn, &text);
        wyoming_close(g_stream_conn);
        g_stream_conn = NULL;

        float dur = (float)g_stream_samples / (float)g_core->sample_rate;
        const char *caller_name = get_caller_name(g_caller_id);

        if (rc == WYOMING_OK && text && text[0])
            emit_transcript(text, caller_name, dur);
        else if (rc != 0)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "streaming transcription failed (%d) for %.1fs",
                        rc, dur);
        free(text);
#endif
    } else {
        /* Batch mode: hand off to worker */
        (void)evt;
        if (!g_capturing) return;
        g_capturing = 0;
        g_core->audio_tap_unregister(batch_audio_tap);

        if (!g_cap_buf || g_cap_len == 0) {
            free(g_cap_buf);
            g_cap_buf = NULL;
            return;
        }

        float dur_ms = (float)g_cap_len / (float)g_core->sample_rate * 1000.0f;
        if (dur_ms < (float)g_min_duration_ms) {
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                        "skipping short TX (%.0fms < %dms)", dur_ms, g_min_duration_ms);
            free(g_cap_buf);
            g_cap_buf = NULL;
            return;
        }

        pthread_mutex_lock(&g_asr_mutex);
        if (g_asr_pending && g_asr_req.pcm)
            free(g_asr_req.pcm);
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
    core->subscribe(KERCHEVT_VCOR_ASSERT, on_vcor_assert, NULL);
    core->subscribe(KERCHEVT_VCOR_DROP, on_vcor_drop, NULL);
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

    v = kerchunk_config_get(cfg, "asr", "mode");
    if (v && strcmp(v, "streaming") == 0)
        g_mode = ASR_MODE_STREAMING;
    else
        g_mode = ASR_MODE_BATCH;

    /* Accept duration suffix (5m, 30s, plain integer = seconds). The
     * old kerchunk_config_get_int parser silently atoi'd "5m" to 5 →
     * 5-second captures when the operator meant 5 minutes. */
    int requested_s = kerchunk_config_get_duration_s(cfg, "asr",
                                                     "max_capture", 30);
    g_max_capture_s = requested_s;
    if (g_max_capture_s > MAX_CAPTURE_S) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
            "max_capture=%ds clamped to hard cap %ds",
            requested_s, MAX_CAPTURE_S);
        g_max_capture_s = MAX_CAPTURE_S;
    }
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

    /* Stop existing worker on reload (batch mode only) */
    if (g_asr_tid >= 0) {
        g_core->thread_stop(g_asr_tid);
        pthread_cond_signal(&g_asr_cond);
        g_core->thread_join(g_asr_tid);
        g_asr_tid = -1;
    }

    /* Batch mode needs a worker thread; streaming mode works inline */
    if (g_mode == ASR_MODE_BATCH) {
        g_asr_pending = 0;
        g_asr_tid = g_core->thread_create("asr-worker", batch_worker, NULL);
        if (g_asr_tid < 0) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create worker thread");
            g_enabled = 0;
            return -1;
        }
    }

#ifdef HAVE_NEMO_NORMALIZE
    /* Initialize inverse text normalization (spoken→written) */
    if (g_itn) { nemo_normalizer_destroy(g_itn); g_itn = NULL; }
    v = kerchunk_config_get(cfg, "asr", "normalize_far_dir");
    if (v) snprintf(g_far_dir, sizeof(g_far_dir), "%s", v);

    /* Default to system-installed FAR grammars */
    if (!g_far_dir[0])
        snprintf(g_far_dir, sizeof(g_far_dir),
                 "/usr/share/nemo-normalize/far_export");

    if (g_far_dir[0]) {
        char classify[600], verbalize[600];
        snprintf(classify, sizeof(classify),
                 "%s/en_itn_grammars_cased/classify/tokenize_and_classify.far",
                 g_far_dir);
        snprintf(verbalize, sizeof(verbalize),
                 "%s/en_itn_grammars_cased/verbalize/verbalize.far",
                 g_far_dir);
        g_itn = nemo_normalizer_create(classify, verbalize, NULL);
        if (g_itn)
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "ITN enabled (nemo_normalize)");
        else
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "ITN init failed — check FAR files in %s", g_far_dir);
    }
#endif

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "ready: %s:%d lang=%s mode=%s max=%ds min=%dms",
                g_host, g_port, g_language,
                g_mode == ASR_MODE_STREAMING ? "streaming" : "batch",
                g_max_capture_s, g_min_duration_ms);
    return 0;
}

static void asr_unload(void)
{
    g_enabled = 0;
    g_capturing = 0;
    g_streaming_active = 0;

    if (g_asr_tid >= 0) {
        g_core->thread_stop(g_asr_tid);
        pthread_cond_signal(&g_asr_cond);
        g_core->thread_join(g_asr_tid);
        g_asr_tid = -1;
    }

    free(g_cap_buf);
    g_cap_buf = NULL;
    free(g_tx_buf);
    g_tx_buf = NULL;
    g_tx_capturing = 0;

#ifdef HAVE_LIBWYOMING
    if (g_stream_conn) {
        wyoming_close(g_stream_conn);
        g_stream_conn = NULL;
    }
#endif

    pthread_mutex_lock(&g_asr_mutex);
    free(g_asr_req.pcm);
    g_asr_req.pcm = NULL;
    pthread_mutex_unlock(&g_asr_mutex);

#ifdef HAVE_NEMO_NORMALIZE
    if (g_itn) { nemo_normalizer_destroy(g_itn); g_itn = NULL; }
#endif

    g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP, on_cor_drop);
    g_core->unsubscribe(KERCHEVT_VCOR_ASSERT, on_vcor_assert);
    g_core->unsubscribe(KERCHEVT_VCOR_DROP, on_vcor_drop);
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
    resp_str(r, "mode", g_mode == ASR_MODE_STREAMING ? "streaming" : "batch");
    resp_str(r, "wyoming_host", g_host);
    resp_int(r, "wyoming_port", g_port);
    resp_str(r, "language", g_language);
    resp_int(r, "max_capture_s", g_max_capture_s);
    resp_int(r, "min_duration_ms", g_min_duration_ms);
    resp_bool(r, "capturing", g_mode == ASR_MODE_STREAMING ? (int)g_streaming_active : (int)g_capturing);
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
        "Modes:\n"
        "  batch     — Accumulates audio, transcribes after COR drop.\n"
        "              Best accuracy (Whisper). ~1-3s delay after unkey.\n\n"
        "  streaming — Feeds audio in real-time during transmission.\n"
        "              Transcript available instantly on COR drop.\n"
        "              Requires streaming model (Zipformer).\n\n"
        "Config: [asr] enabled, mode, wyoming_host, wyoming_port,\n"
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
      .ui_type = CLI_UI_BUTTON, .ui_command = "asr",
      .subcommands = "history" },
};

static kerchunk_module_def_t mod_asr = {
    .name             = "mod_asr",
    .version          = "2.0.0",
    .description      = "Automatic speech recognition (Wyoming ASR)",
    .load             = asr_load,
    .configure        = asr_configure,
    .unload           = asr_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_asr);
