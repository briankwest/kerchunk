/*
 * mod_recorder.c — Transmission recorder
 *
 * Records every RX and TX transmission to timestamped WAV files.
 * Uses the audio tap to capture frames during active transmissions.
 *
 * Filenames: recordings/20260318_215530_RX_Brian.wav
 *            recordings/20260318_215545_TX_weather.wav
 *
 * Config: [recording] section in kerchunk.conf
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdatomic.h>
#include <time.h>

#define LOG_MOD "recorder"
#define MAX_DURATION_S 300  /* 5 min hard cap */

static kerchunk_core_t *g_core;

/* Config */
static int  g_enabled = 0;
static char g_dir[256] = "recordings";
static int  g_max_duration_s = 300;

/* RX recording state (g_rx_active read by audio thread tap) */
static atomic_int g_rx_active;
static int16_t *g_rx_buf;
static size_t   g_rx_len;
static size_t   g_rx_cap;
static char     g_rx_start_time[20];   /* YYYYMMDD_HHMMSS */
static int      g_rx_caller_id;

/* TX recording state (g_tx_active read by audio thread tap) */
static atomic_int g_tx_active;
static int16_t *g_tx_buf;
static size_t   g_tx_len;
static size_t   g_tx_cap;
static char     g_tx_start_time[32];

/* ---- helpers ---- */

static void fmt_timestamp(char *buf, size_t max)
{
    time_t now = time(NULL);
    struct tm tbuf;
    struct tm *t = localtime_r(&now, &tbuf);
    snprintf(buf, max, "%04d%02d%02d_%02d%02d%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
}

static void sanitize_name(char *out, size_t max, const char *name)
{
    size_t i = 0;
    for (const char *p = name; *p && i < max - 1; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')
            out[i++] = *p;
    }
    out[i] = '\0';
}

static void append_samples(int16_t **buf, size_t *len, size_t *cap,
                           const int16_t *samples, size_t n)
{
    size_t max_samples = (size_t)g_core->sample_rate * (size_t)g_max_duration_s;
    if (*len >= max_samples)
        return;

    size_t needed = *len + n;
    if (needed > *cap) {
        size_t new_cap = *cap * 2;
        if (new_cap < needed) new_cap = needed + (size_t)g_core->sample_rate;
        if (new_cap > max_samples) new_cap = max_samples;
        int16_t *new_buf = realloc(*buf, new_cap * sizeof(int16_t));
        if (!new_buf) return;
        *buf = new_buf;
        *cap = new_cap;
    }

    size_t to_copy = n;
    if (*len + to_copy > max_samples)
        to_copy = max_samples - *len;
    memcpy(*buf + *len, samples, to_copy * sizeof(int16_t));
    *len += to_copy;
}

static void activity_log(const char *direction, const char *who,
                         float duration, const char *wav_path)
{
    char log_path[512];
    snprintf(log_path, sizeof(log_path), "%s/activity.log", g_dir);

    FILE *fp = fopen(log_path, "a");
    if (!fp) return;

    time_t now = time(NULL);
    struct tm tbuf;
    struct tm *t = localtime_r(&now, &tbuf);
    fprintf(fp, "%04d-%02d-%02d %02d:%02d:%02d %s %s %.1fs %s\n",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec,
            direction, who, duration, wav_path);
    fclose(fp);
}

/* Static buffer for RECORDING_SAVED event path — survives event dispatch */
static char g_last_saved_path[512];

static void save_recording(const char *direction, const char *start_time,
                           const char *who, int16_t *buf, size_t len)
{
    if (!buf || len == 0) return;

    char safe_who[32];
    sanitize_name(safe_who, sizeof(safe_who), who);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%s_%s.wav",
             g_dir, start_time, direction, safe_who);

    float dur = (float)len / (float)g_core->sample_rate;

    if (kerchunk_wav_write(path, buf, len, g_core->sample_rate) == 0) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                     "saved %s recording: %s (%.1fs, %zu samples)",
                     direction, path, dur, len);
        activity_log(direction, safe_who, dur, path);

        /* Notify other modules (e.g., mod_cdr) — use static buffer
         * so the path pointer remains valid during event dispatch */
        snprintf(g_last_saved_path, sizeof(g_last_saved_path), "%s", path);
        kerchevt_t rev = {
            .type = KERCHEVT_RECORDING_SAVED,
            .recording = {
                .path      = g_last_saved_path,
                .direction = direction,
                .user_id   = (strcmp(direction, "RX") == 0) ? g_rx_caller_id : 0,
                .duration  = dur,
            },
        };
        g_core->fire_event(&rev);
    } else {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                     "failed to save: %s", path);
    }
}

/* ---- RX recording (incoming audio via tap) ---- */

static void rx_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_rx_active || !evt->audio.samples)
        return;
    append_samples(&g_rx_buf, &g_rx_len, &g_rx_cap,
                   evt->audio.samples, evt->audio.n);
}

static void rx_start(void)
{
    if (g_rx_active) return;
    g_rx_active = 1;
    g_rx_len = 0;
    g_rx_cap = (size_t)g_core->sample_rate * 30;  /* start with 30s buffer */
    g_rx_buf = malloc(g_rx_cap * sizeof(int16_t));
    if (!g_rx_buf) { g_rx_active = 0; return; }
    fmt_timestamp(g_rx_start_time, sizeof(g_rx_start_time));
    /* Don't reset g_rx_caller_id here — CALLER_IDENTIFIED fires before
     * COR_ASSERT (login session re-identifies during COR debounce).
     * Resetting here would wipe the identification. It's reset in rx_stop()
     * after the recording is saved. */
    g_core->audio_tap_register(rx_audio_tap, NULL);
}

static void rx_stop(void)
{
    if (!g_rx_active) return;
    g_rx_active = 0;
    g_core->audio_tap_unregister(rx_audio_tap);

    /* Determine caller username for filename */
    const char *who = "unknown";
    if (g_rx_caller_id > 0) {
        const kerchunk_user_t *u = g_core->user_lookup_by_id(g_rx_caller_id);
        if (u) who = u->username;
    }

    save_recording("RX", g_rx_start_time, who, g_rx_buf, g_rx_len);
    free(g_rx_buf);
    g_rx_buf = NULL;
    g_rx_len = 0;
    g_rx_caller_id = 0;  /* reset for next transmission */
    g_rx_cap = 0;
}

/* ---- TX recording (via playback tap) ---- */

static void tx_playback_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_tx_active || !evt->audio.samples)
        return;
    append_samples(&g_tx_buf, &g_tx_len, &g_tx_cap,
                   evt->audio.samples, evt->audio.n);
}

static void tx_start(void)
{
    if (g_tx_active) return;
    g_tx_active = 1;
    g_tx_len = 0;
    g_tx_cap = (size_t)g_core->sample_rate * 30;
    g_tx_buf = malloc(g_tx_cap * sizeof(int16_t));
    if (!g_tx_buf) { g_tx_active = 0; return; }
    fmt_timestamp(g_tx_start_time, sizeof(g_tx_start_time));
    g_core->playback_tap_register(tx_playback_tap, NULL);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "TX recording started");
}

static void tx_stop(void)
{
    if (!g_tx_active) return;
    g_tx_active = 0;
    g_core->playback_tap_unregister(tx_playback_tap);

    save_recording("TX", g_tx_start_time, "transmit", g_tx_buf, g_tx_len);
    free(g_tx_buf);
    g_tx_buf = NULL;
    g_tx_len = 0;
    g_tx_cap = 0;
}

/* ---- event handlers ---- */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;
    rx_start();
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;
    rx_stop();
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_rx_caller_id = evt->caller.user_id;
}

static void on_queue_drain(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;

    /* Start TX recording when queue actually starts draining audio.
     * This avoids empty recordings from manual PTT with no audio. */
    if (!g_tx_active)
        tx_start();
}

static void on_queue_complete(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;
    tx_stop();
}

/* ---- module lifecycle ---- */

static int recorder_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_COR_ASSERT, on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,   on_cor_drop, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    core->subscribe(KERCHEVT_QUEUE_DRAIN,    on_queue_drain, NULL);
    core->subscribe(KERCHEVT_QUEUE_COMPLETE, on_queue_complete, NULL);
    return 0;
}

static int recorder_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "recording", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "recording", "directory");
    if (v) snprintf(g_dir, sizeof(g_dir), "%s", v);

    /* max_duration: uses standard duration parser (bare digits = ms,
     * or use suffixes: 300s, 5m, 1h30m, etc).  Default 5m. */
    g_max_duration_s = kerchunk_config_get_duration_ms(cfg, "recording", "max_duration", MAX_DURATION_S * 1000) / 1000;
    if (g_max_duration_s > MAX_DURATION_S) g_max_duration_s = MAX_DURATION_S;
    if (g_max_duration_s < 1) g_max_duration_s = 1;

    if (g_enabled) {
        if (mkdir(g_dir, 0755) != 0 && errno != EEXIST)
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "cannot create directory: %s", g_dir);
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "enabled=%d dir=%s max_duration=%ds",
                g_enabled, g_dir, g_max_duration_s);
    return 0;
}

static void recorder_unload(void)
{
    rx_stop();
    tx_stop();

    g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,   on_cor_drop);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_QUEUE_DRAIN,    on_queue_drain);
    g_core->unsubscribe(KERCHEVT_QUEUE_COMPLETE, on_queue_complete);
}

/* CLI */
static int cli_recorder(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    resp_bool(r, "enabled", g_enabled);
    resp_str(r, "directory", g_dir);
    resp_int(r, "max_duration_s", g_max_duration_s);
    resp_bool(r, "rx_active", g_rx_active);
    if (g_rx_active)
        resp_float(r, "rx_duration_s", (double)g_rx_len / (double)g_core->sample_rate);
    return 0;

usage:
    resp_text_raw(r, "Transmission recorder\n\n"
        "  recorder\n"
        "    Show recording status and configuration.\n\n"
        "    Fields:\n"
        "      enabled         Whether recording is active\n"
        "      directory       Path to WAV output directory\n"
        "      max_duration_s  Maximum recording length per transmission\n"
        "      rx_active       Whether an RX recording is in progress\n"
        "      rx_duration_s   Current RX recording elapsed time\n\n"
        "    Records every RX and TX transmission to timestamped WAV files.\n"
        "    Filenames: <dir>/YYYYMMDD_HHMMSS_<RX|TX>_<user>.wav\n"
        "    Also maintains an activity.log with all recordings.\n\n"
        "Config: [recording] enabled, directory, max_duration\n");
    resp_str(r, "error", "usage: recorder [help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "recorder", "recorder", "Recording status", cli_recorder },
};

static kerchunk_module_def_t mod_recorder = {
    .name             = "mod_recorder",
    .version          = "1.0.0",
    .description      = "Transmission recorder",
    .load             = recorder_load,
    .configure        = recorder_configure,
    .unload           = recorder_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_recorder);
