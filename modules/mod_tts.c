/*
 * mod_tts.c — Text-to-speech service via ElevenLabs API
 *
 * Provides g_core->tts_speak(text, priority) for other modules.
 * Synthesis runs on a background thread to avoid blocking the main loop.
 * POSTs text to ElevenLabs, receives PCM at 16kHz, decimates to 8kHz,
 * and queues for playback.
 *
 * Responses are cached as WAV files keyed by text hash. Identical text
 * is synthesized once and replayed from disk on subsequent requests.
 * Cache dir: <sounds_dir>/cache/tts/
 *
 * Config: [tts] section in kerchunk.conf
 * Requires: libcurl, pthreads
 */

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
    uint32_t h = fnv1a(text);
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

/* ── ElevenLabs synthesis (runs on worker thread) ── */

static void synthesize_and_queue(const char *text, int priority)
{
#ifdef HAVE_NEMO_NORMALIZE
    /* Normalize text before cache lookup and synthesis */
    char normalized[1024];
    if (g_normalizer) {
        if (nemo_normalize(g_normalizer, text, normalized, (int)sizeof(normalized)) == 0) {
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                        "normalized: \"%s\" -> \"%s\"", text, normalized);
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
    if (!buf.data) { curl_slist_free_all(headers); return; }

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(buf.data);
        curl_slist_free_all(headers);
        return;
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
        return;
    }

    if (http_code != 200) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "ElevenLabs API error %ld: %.100s",
                    http_code, buf.data);
        free(buf.data);
        return;
    }

    size_t src_samples = buf.len / sizeof(int16_t);
    if (src_samples == 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "empty audio response");
        free(buf.data);
        return;
    }

    int16_t *resampled = NULL;
    size_t out_len = 0;
    if (kerchunk_resample((const int16_t *)buf.data, src_samples,
                          ELEVENLABS_PCM_RATE, g_core->sample_rate,
                          &resampled, &out_len) != 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "resample failed");
        free(buf.data);
        return;
    }
    free(buf.data);
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
                            "cached: %s (%zu samples, %.1fs)",
                            path, out_len, (float)out_len / g_core->sample_rate);
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
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                    "spoke %zu samples (%.1fs): %s",
                    out_len, (float)out_len / g_core->sample_rate, text);
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

    v = kerchunk_config_get(cfg, "tts", "api_key");
    if (v) snprintf(g_api_key, sizeof(g_api_key), "%s", v);

    v = kerchunk_config_get(cfg, "tts", "voice_id");
    if (v) snprintf(g_voice_id, sizeof(g_voice_id), "%s", v);

    v = kerchunk_config_get(cfg, "tts", "model");
    if (v) snprintf(g_model, sizeof(g_model), "%s", v);

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

    if (g_far_dir[0]) {
        char classify[600], verbalize[600];
        snprintf(classify, sizeof(classify),
                 "%s/en_tn_grammars_cased/classify/tokenize_and_classify.far", g_far_dir);
        snprintf(verbalize, sizeof(verbalize),
                 "%s/en_tn_grammars_cased/verbalize/verbalize.far", g_far_dir);
        g_normalizer = nemo_normalizer_create(classify, verbalize, NULL);
        if (g_normalizer)
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "text normalization enabled (FAR: %s)", g_far_dir);
        else
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "text normalization init failed — check FAR files in %s", g_far_dir);
    }
#endif

    if (g_api_key[0] == '\0') {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "no api_key configured — TTS disabled");
        return 0;
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
        resp_str(r, "status", g_initialized ? "ready" : "no api_key");
        resp_str(r, "engine", "ElevenLabs (async)");
        resp_str(r, "voice_id", g_voice_id);
        resp_str(r, "model", g_model);
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
    resp_text_raw(r, "Text-to-speech via ElevenLabs API (cached)\n\n"
        "  tts say <text>\n"
        "    Synthesize and play the given text on-air.\n"
        "    text: one or more words to speak\n\n"
        "  tts status\n"
        "    Show TTS engine status: voice, model, cache directory,\n"
        "    pending queue depth, normalization state.\n\n"
        "  tts cache-clear\n"
        "    Remove all cached WAV files from the TTS cache directory.\n"
        "    Forces re-synthesis on next request for each phrase.\n\n"
        "    Synthesis runs on a background thread. Responses are cached\n"
        "    as WAV files keyed by FNV-1a text hash. Identical text is\n"
        "    synthesized once and replayed from disk on subsequent calls.\n\n"
        "Config: [tts] api_key, voice_id, model\n"
        "        [general] sounds_dir (cache stored in <sounds_dir>/cache/tts/)\n");
    resp_str(r, "error", "usage: tts <say <text>|status|cache-clear>");
    resp_finish(r);
    return -1;
}

static const kerchunk_ui_field_t tts_fields[] = {
    { "text", "Text", "text", NULL, "Hello world" },
};

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "tts", .usage = "tts say <text> | status | cache-clear",
      .description = "Text-to-speech (ElevenLabs)", .handler = cli_tts,
      .category = "Announcements", .ui_label = "TTS Speak", .ui_type = CLI_UI_FORM,
      .ui_command = "tts say", .ui_fields = tts_fields, .num_ui_fields = 1 },
};

static kerchunk_module_def_t mod_tts = {
    .name             = "mod_tts",
    .version          = "1.0.0",
    .description      = "Text-to-speech via ElevenLabs API (cached)",
    .load             = tts_load,
    .configure        = tts_configure,
    .unload           = tts_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_tts);
