/*
 * mod_tts.c — Text-to-speech service
 *
 * Provides g_core->tts_speak(text, priority) for other modules.
 * Synthesis runs on a background thread to avoid blocking the main loop.
 *
 * Engines:
 *   elevenlabs — POSTs text to ElevenLabs API, receives PCM at 16kHz.
 *   wyoming    — Connects to Wyoming protocol server (e.g. wyoming-piper)
 *                via TCP. No subprocess, network-capable, HA-compatible.
 *
 * Both engines resample to g_core->sample_rate before playback.
 *
 * Responses are cached as WAV files keyed by text hash. Identical text
 * is synthesized once and replayed from disk on subsequent requests.
 * Cache dir: <sounds_dir>/cache/tts/
 *
 * Config: [tts] section in kerchunk.conf
 * Requires: libcurl (ElevenLabs), pthreads
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "kerchunk_wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <curl/curl.h>

#ifdef HAVE_NEMO_NORMALIZE
#include "nemo_normalize.h"
#endif

#ifdef HAVE_LIBWYOMING
#include <libwyoming/wyoming.h>
#endif

#define LOG_MOD "tts"
#define MAX_TTS_SAMPLES (g_core->sample_rate * 60)
#define ELEVENLABS_PCM_RATE 16000
#define TTS_QUEUE_SIZE 8

static kerchunk_core_t *g_core;

/* Config */
static char g_api_key[128]  = "";

#ifdef HAVE_NEMO_NORMALIZE
static NemoNormalizer *g_normalizer = NULL;
static char g_far_dir[512] = "";
#endif
static char g_voice_id[64]  = "21m00Tcm4TlvDq8ikWAM";
static char g_model[64]     = "eleven_turbo_v2_5";
static char g_cache_dir[512] = "";
static volatile int g_initialized = 0;

/* Engine selection */
typedef enum { TTS_ELEVENLABS, TTS_WYOMING } tts_engine_t;
static tts_engine_t g_engine      = TTS_ELEVENLABS;

/* Wyoming engine */
static char g_wyoming_host[64]    = "127.0.0.1";
static int  g_wyoming_port        = 10200;
static char g_wyoming_voice[64]   = "";

/* ── Background worker thread ── */

typedef struct {
    char text[2048];
    int  priority;
} tts_request_t;

static tts_request_t   g_tts_queue[TTS_QUEUE_SIZE];
static int             g_tts_head;
static int             g_tts_tail;
static int             g_tts_count;
static pthread_mutex_t g_tts_mutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_tts_cond     = PTHREAD_COND_INITIALIZER;
static int             g_tts_tid = -1;

/* ── FNV-1a hash for cache keys ── */

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

static void cache_path_for_text(const char *text, char *out, size_t outsz)
{
    /* Include engine + voice info so different engines get separate cache entries */
    char key[3072];
    if (g_engine == TTS_WYOMING)
        snprintf(key, sizeof(key), "wyoming:%s:%d:%s:%s",
                 g_wyoming_host, g_wyoming_port, g_wyoming_voice, text);
    else
        snprintf(key, sizeof(key), "el:%s:%s:%s", g_voice_id, g_model, text);
    uint32_t h = fnv1a(key);
    snprintf(out, outsz, "%s/%08x_%zu.wav", g_cache_dir, h, strlen(text));
}

/* ── libcurl response buffer ── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} curl_buf_t;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    curl_buf_t *buf = ud;
    if (size > 0 && nmemb > SIZE_MAX / size) return 0;
    size_t total = size * nmemb;
    if (buf->len + total + 1 > buf->cap) {
        size_t new_cap = buf->cap * 2;
        if (new_cap < buf->len + total + 1)
            new_cap = buf->len + total + 1;
        char *p = realloc(buf->data, new_cap);
        if (!p) return 0;
        buf->data = p;
        buf->cap  = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    return total;
}

/* ── JSON-safe escaping ── */

static size_t json_escape(const char *text, char *out, size_t max)
{
    size_t j = 0;
    for (size_t i = 0; text[i] && j < max - 6; i++) {
        switch (text[i]) {
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:
            if ((unsigned char)text[i] < 0x20) continue;
            out[j++] = text[i];
            break;
        }
    }
    out[j] = '\0';
    return j;
}

/* ── ElevenLabs synthesis ── */

static int elevenlabs_synthesize(const char *text, int16_t **out, size_t *out_len)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.elevenlabs.io/v1/text-to-speech/%s"
             "?output_format=pcm_16000",
             g_voice_id);

    char escaped[1024];
    json_escape(text, escaped, sizeof(escaped));

    char body[2048];
    snprintf(body, sizeof(body),
             "{\"text\":\"%s\",\"model_id\":\"%s\"}",
             escaped, g_model);

    struct curl_slist *headers = NULL;
    char auth[256];
    snprintf(auth, sizeof(auth), "xi-api-key: %s", g_api_key);
    headers = curl_slist_append(headers, auth);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/octet-stream");

    curl_buf_t buf = { .data = malloc(65536), .len = 0, .cap = 65536 };
    if (!buf.data) { curl_slist_free_all(headers); return -1; }

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(buf.data);
        curl_slist_free_all(headers);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, 50L * 1024 * 1024);  /* 50MB for audio */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "curl failed: %s",
                    curl_easy_strerror(res));
        free(buf.data);
        return -1;
    }

    if (http_code != 200) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "ElevenLabs API error %ld: %.100s",
                    http_code, buf.data);
        free(buf.data);
        return -1;
    }

    size_t samples = buf.len / sizeof(int16_t);
    if (samples == 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "empty audio response");
        free(buf.data);
        return -1;
    }

    *out     = (int16_t *)buf.data;
    *out_len = samples;
    return 0;
}

/* ── Wyoming synthesis (libwyoming TCP client) ── */

#ifdef HAVE_LIBWYOMING
static int wyoming_tts_synthesize(const char *text, int16_t **out,
                                   size_t *out_len, int *out_rate)
{
    wyoming_conn_t *conn = wyoming_connect(g_wyoming_host,
                                            (uint16_t)g_wyoming_port);
    if (!conn) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "wyoming: connect failed: %s:%d", g_wyoming_host, g_wyoming_port);
        return -1;
    }

    const char *voice = g_wyoming_voice[0] ? g_wyoming_voice : NULL;
    wyoming_audio_format_t fmt = {0};
    int16_t *pcm = NULL;
    size_t samples = 0;

    wyoming_error_t rc = wyoming_synthesize_pcm(conn, text, voice,
                                                 &pcm, &samples, &fmt);
    wyoming_close(conn);

    if (rc != WYOMING_OK || !pcm || samples == 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "wyoming: synthesis failed (%d)", rc);
        free(pcm);
        return -1;
    }

    *out      = pcm;
    *out_len  = samples;
    *out_rate = fmt.rate;
    return 0;
}
#endif

/* ── Synthesis dispatcher (runs on worker thread) ── */

static void synthesize_and_queue(const char *text, int priority)
{
#ifdef HAVE_NEMO_NORMALIZE
    /* Normalize text before cache lookup and synthesis */
    char normalized[1024];
    if (g_normalizer) {
        if (nemo_normalize(g_normalizer, text, normalized, (int)sizeof(normalized)) == 0
            && normalized[0]) {
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "TN: \"%s\" -> \"%s\"", text, normalized);
            text = normalized;
        }
    }
#endif

    /* Check cache first */
    if (g_cache_dir[0]) {
        char path[576];
        cache_path_for_text(text, path, sizeof(path));
        if (access(path, R_OK) == 0) {
            g_core->queue_audio_file(path, priority);
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "cache hit: %s", text);
            kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
                .announcement = { .source = "tts", .description = text } };
            kerchevt_fire(&ae);
            return;
        }
    }

    /* Synthesize via selected engine */
    int16_t *raw_pcm = NULL;
    size_t   raw_samples = 0;
    int      src_rate = 0;

#ifdef HAVE_LIBWYOMING
    if (g_engine == TTS_WYOMING) {
        if (wyoming_tts_synthesize(text, &raw_pcm, &raw_samples, &src_rate) != 0)
            return;
    } else
#endif
    {
        if (elevenlabs_synthesize(text, &raw_pcm, &raw_samples) != 0)
            return;
        src_rate = ELEVENLABS_PCM_RATE;
    }

    /* Resample to system rate */
    int16_t *resampled = NULL;
    size_t out_len = 0;
    if (kerchunk_resample(raw_pcm, raw_samples,
                          src_rate, g_core->sample_rate,
                          &resampled, &out_len) != 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "resample failed");
        free(raw_pcm);
        return;
    }
    free(raw_pcm);
    if (out_len > (size_t)MAX_TTS_SAMPLES) out_len = (size_t)MAX_TTS_SAMPLES;

    if (out_len > 0) {
        /* Write cache file (atomic: temp + rename) */
        if (g_cache_dir[0]) {
            char path[576];
            cache_path_for_text(text, path, sizeof(path));
            char tmp[580];
            snprintf(tmp, sizeof(tmp), "%s.tmp", path);
            if (kerchunk_wav_write(tmp, resampled, out_len, g_core->sample_rate) == 0) {
                rename(tmp, path);  /* atomic */
                g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                            "synthesized (%.1fs): %s",
                            (float)out_len / g_core->sample_rate, text);
                /* Queue from file so buffer can be freed immediately */
                g_core->queue_audio_file(path, priority);
                free(resampled);
                kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
                    .announcement = { .source = "tts", .description = text } };
                kerchevt_fire(&ae);
                return;
            }
            /* Cache write failed — fall through to queue from buffer */
        }

        g_core->queue_audio_buffer(resampled, out_len, priority, 0);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "synthesized (%.1fs): %s",
                    (float)out_len / g_core->sample_rate, text);
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "tts", .description = text } };
        kerchevt_fire(&ae);
    }

    free(resampled);
}

/* ── Worker thread ── */

static void *tts_worker(void *arg)
{
    (void)arg;

    while (!g_core->thread_should_stop(g_tts_tid)) {
        pthread_mutex_lock(&g_tts_mutex);
        while (g_tts_count == 0 && !g_core->thread_should_stop(g_tts_tid)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_tts_cond, &g_tts_mutex, &ts);
        }

        if (g_core->thread_should_stop(g_tts_tid)) {
            pthread_mutex_unlock(&g_tts_mutex);
            break;
        }

        /* Dequeue request */
        tts_request_t req = g_tts_queue[g_tts_head];
        g_tts_head = (g_tts_head + 1) % TTS_QUEUE_SIZE;
        g_tts_count--;
        pthread_mutex_unlock(&g_tts_mutex);

        synthesize_and_queue(req.text, req.priority);
    }

    return NULL;
}

/* ── TTS speak (called from any thread, non-blocking) ── */

static int tts_speak_impl(const char *text, int priority)
{
    if (!g_initialized || !text || !text[0])
        return -1;

    pthread_mutex_lock(&g_tts_mutex);
    if (g_tts_count >= TTS_QUEUE_SIZE) {
        pthread_mutex_unlock(&g_tts_mutex);
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "TTS queue full, dropping");
        return -1;
    }

    tts_request_t *req = &g_tts_queue[g_tts_tail];
    snprintf(req->text, sizeof(req->text), "%s", text);
    req->priority = priority;
    g_tts_tail = (g_tts_tail + 1) % TTS_QUEUE_SIZE;
    g_tts_count++;
    pthread_cond_signal(&g_tts_cond);
    pthread_mutex_unlock(&g_tts_mutex);

    return 0;
}

/* ── Module lifecycle ── */

static int tts_load(kerchunk_core_t *core)
{
    g_core = core;
    return 0;
}

static int tts_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    /* Engine selection */
    v = kerchunk_config_get(cfg, "tts", "engine");
    if (v) {
        if (strcmp(v, "wyoming") == 0)
            g_engine = TTS_WYOMING;
        else
            g_engine = TTS_ELEVENLABS;
    }

    /* ElevenLabs settings */
    v = kerchunk_config_get(cfg, "tts", "api_key");
    if (v) snprintf(g_api_key, sizeof(g_api_key), "%s", v);

    v = kerchunk_config_get(cfg, "tts", "voice_id");
    if (v) snprintf(g_voice_id, sizeof(g_voice_id), "%s", v);

    v = kerchunk_config_get(cfg, "tts", "model");
    if (v) snprintf(g_model, sizeof(g_model), "%s", v);

    /* Wyoming settings */
    v = kerchunk_config_get(cfg, "tts", "wyoming_host");
    if (v) snprintf(g_wyoming_host, sizeof(g_wyoming_host), "%s", v);

    v = kerchunk_config_get(cfg, "tts", "wyoming_port");
    if (v) g_wyoming_port = atoi(v);

    v = kerchunk_config_get(cfg, "tts", "wyoming_voice");
    if (v) snprintf(g_wyoming_voice, sizeof(g_wyoming_voice), "%s", v);

    /* Set up cache directory */
    const char *sdir = kerchunk_config_get(cfg, "general", "sounds_dir");
    if (!sdir) sdir = "./sounds";
    snprintf(g_cache_dir, sizeof(g_cache_dir), "%s/cache/tts", sdir);
    {
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/cache", sdir);
        if (mkdir(parent, 0755) != 0 && errno != EEXIST)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "mkdir %s: %s", parent, strerror(errno));
        if (mkdir(g_cache_dir, 0755) != 0 && errno != EEXIST)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "mkdir %s: %s", g_cache_dir, strerror(errno));
    }

#ifdef HAVE_NEMO_NORMALIZE
    /* Initialize text normalizer */
    if (g_normalizer) {
        nemo_normalizer_destroy(g_normalizer);
        g_normalizer = NULL;
    }
    const char *far_dir = kerchunk_config_get(cfg, "tts", "normalize_far_dir");
    if (far_dir) snprintf(g_far_dir, sizeof(g_far_dir), "%s", far_dir);

    /* Default to system-installed FAR grammars */
    if (!g_far_dir[0])
        snprintf(g_far_dir, sizeof(g_far_dir),
                 "/usr/share/nemo-normalize/far_export");

    if (g_far_dir[0]) {
        char classify[600], verbalize[600];
        snprintf(classify, sizeof(classify),
                 "%s/en_tn_grammars_cased/classify/tokenize_and_classify.far", g_far_dir);
        snprintf(verbalize, sizeof(verbalize),
                 "%s/en_tn_grammars_cased/verbalize/verbalize.far", g_far_dir);
        g_normalizer = nemo_normalizer_create(classify, verbalize, NULL);
        if (g_normalizer)
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "TN enabled (nemo_normalize)");
        else
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "TN init failed — check FAR files in %s", g_far_dir);
    }
#endif

    /* Validate engine-specific requirements */
    if (g_engine == TTS_WYOMING) {
#ifndef HAVE_LIBWYOMING
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "wyoming engine selected but libwyoming not available — TTS disabled");
        return 0;
#else
        if (g_wyoming_port <= 0 || g_wyoming_port > 65535) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "invalid wyoming_port %d — TTS disabled", g_wyoming_port);
            return 0;
        }
#endif
    } else {
        if (g_api_key[0] == '\0') {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "no api_key configured — TTS disabled");
            return 0;
        }
    }

    /* Stop existing worker thread on reload */
    if (g_tts_tid >= 0) {
        g_core->thread_stop(g_tts_tid);
        pthread_cond_signal(&g_tts_cond);
        g_core->thread_join(g_tts_tid);
        g_tts_tid = -1;
    }

    /* Start worker thread */
    g_tts_head = g_tts_tail = g_tts_count = 0;
    g_tts_tid = g_core->thread_create("tts-synth", tts_worker, NULL);
    if (g_tts_tid < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "failed to create TTS worker thread");
        return -1;
    }

    g_initialized = 1;
    g_core->tts_speak = tts_speak_impl;

    if (g_engine == TTS_WYOMING)
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "Wyoming TTS ready: %s:%d voice=%s cache=%s",
                    g_wyoming_host, g_wyoming_port,
                    g_wyoming_voice[0] ? g_wyoming_voice : "(default)",
                    g_cache_dir);
    else
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "ElevenLabs TTS ready: voice=%s model=%s cache=%s",
                    g_voice_id, g_model, g_cache_dir);
    return 0;
}

static void tts_unload(void)
{
    if (g_tts_tid >= 0) {
        g_core->thread_stop(g_tts_tid);
        pthread_cond_signal(&g_tts_cond);
        g_core->thread_join(g_tts_tid);
        g_tts_tid = -1;
    }

    g_initialized = 0;
    if (g_core)
        g_core->tts_speak = NULL;

#ifdef HAVE_NEMO_NORMALIZE
    if (g_normalizer) {
        nemo_normalizer_destroy(g_normalizer);
        g_normalizer = NULL;
    }
#endif
}

/* ── CLI ── */

static int cli_tts(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 3 && strcmp(argv[1], "say") == 0) {
        char text[2048] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(text, " ", sizeof(text) - strlen(text) - 1);
            strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
        }
        if (tts_speak_impl(text, KERCHUNK_PRI_NORMAL) == 0) {
            resp_bool(r, "ok", 1);
            resp_str(r, "text", text);
        } else {
            resp_str(r, "error", "TTS failed (check api_key)");
        }
    } else if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        pthread_mutex_lock(&g_tts_mutex);
        int pending = g_tts_count;
        pthread_mutex_unlock(&g_tts_mutex);
        resp_str(r, "status", g_initialized ? "ready" : "disabled");
        if (g_engine == TTS_WYOMING) {
            resp_str(r, "engine", "Wyoming (network)");
            resp_str(r, "wyoming_host", g_wyoming_host);
            resp_int(r, "wyoming_port", g_wyoming_port);
            resp_str(r, "wyoming_voice", g_wyoming_voice[0] ? g_wyoming_voice : "(default)");
        } else {
            resp_str(r, "engine", "ElevenLabs (cloud)");
            resp_str(r, "voice_id", g_voice_id);
            resp_str(r, "model", g_model);
        }
        resp_int(r, "pending", pending);
        if (g_cache_dir[0])
            resp_str(r, "cache_dir", g_cache_dir);
#ifdef HAVE_NEMO_NORMALIZE
        resp_bool(r, "normalize", g_normalizer != NULL);
#endif
    } else if (argc >= 2 && strcmp(argv[1], "cache-clear") == 0) {
        /* Remove all cached WAV files */
        if (g_cache_dir[0]) {
            DIR *d = opendir(g_cache_dir);
            if (d) {
                struct dirent *de;
                int count = 0;
                while ((de = readdir(d)) != NULL) {
                    size_t len = strlen(de->d_name);
                    if (len > 4 && strcmp(de->d_name + len - 4, ".wav") == 0) {
                        char path[768];
                        snprintf(path, sizeof(path), "%s/%s", g_cache_dir, de->d_name);
                        unlink(path);
                        count++;
                    }
                }
                closedir(d);
                resp_bool(r, "ok", 1);
                resp_str(r, "action", "cache cleared");
                resp_int(r, "files_removed", count);
            } else {
                resp_str(r, "error", "Failed to clear cache");
            }
        } else {
            resp_str(r, "error", "No cache directory configured");
        }
    } else {
        goto usage;
    }
    return 0;

usage:
    resp_text_raw(r, "Text-to-speech (ElevenLabs cloud / Piper local)\n\n"
        "  tts say <text>\n"
        "    Synthesize and play the given text on-air.\n"
        "    text: one or more words to speak\n\n"
        "  tts status\n"
        "    Show TTS engine status, pending queue depth,\n"
        "    and normalization state.\n\n"
        "  tts cache-clear\n"
        "    Remove all cached WAV files from the TTS cache directory.\n"
        "    Forces re-synthesis on next request for each phrase.\n\n"
        "    Synthesis runs on a background thread. Responses are cached\n"
        "    as WAV files keyed by FNV-1a text hash. Identical text is\n"
        "    synthesized once and replayed from disk on subsequent calls.\n\n"
        "Config: [tts] engine (elevenlabs|wyoming)\n"
        "  ElevenLabs: api_key, voice_id, model\n"
        "  Wyoming:    wyoming_host, wyoming_port, wyoming_voice\n"
        "  [general]   sounds_dir (cache stored in <sounds_dir>/cache/tts/)\n");
    resp_str(r, "error", "usage: tts <say <text>|status|cache-clear>");
    resp_finish(r);
    return -1;
}

static const kerchunk_ui_field_t tts_fields[] = {
    { "text", "Text", "text", NULL, "Hello world" },
};

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "tts", .usage = "tts say <text> | status | cache-clear",
      .description = "Text-to-speech (ElevenLabs cloud / Piper local)", .handler = cli_tts,
      .category = "Announcements", .ui_label = "TTS Speak", .ui_type = CLI_UI_FORM,
      .ui_command = "tts say", .ui_fields = tts_fields, .num_ui_fields = 1,
      .subcommands = "say,status,cache-clear" },
};

static kerchunk_module_def_t mod_tts = {
    .name             = "mod_tts",
    .version          = "1.1.0",
    .description      = "Text-to-speech (ElevenLabs / Piper / Wyoming)",
    .load             = tts_load,
    .configure        = tts_configure,
    .unload           = tts_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_tts);
