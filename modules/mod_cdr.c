/*
 * mod_cdr.c — Call Detail Records
 *
 * Logs every transmission to a structured CSV file and maintains
 * running statistics. Each COR assert/drop cycle is one "call."
 * Recording filename comes from KERCHEVT_RECORDING_SAVED fired by
 * mod_recorder — no filename guessing.
 *
 * CSV format:
 *   timestamp,date,time,user_id,user_name,method,duration_s,
 *   emergency,recording
 *
 * File: cdr/YYYY-MM-DD.csv (one file per day, auto-rotated)
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#define LOG_MOD "cdr"

static kerchunk_core_t *g_core;

/* Config */
static char g_dir[256] = "cdr";
static int  g_enabled  = 1;

/* Current call state */
static int      g_in_call;
static time_t   g_call_start;
static int      g_call_user_id;
static char     g_call_user_name[32];
static char     g_call_method[16];
static int      g_call_emergency;
static char     g_call_recording[512]; /* Set by RECORDING_SAVED event */

/* Signal quality — accumulated from audio tap during each call */
static int64_t  g_sq_sum;        /* sum of squared samples */
static int64_t  g_sq_count;      /* total samples processed */
static int32_t  g_sq_peak_rms;   /* peak frame RMS in this call */

/* Daily stats */
static int      g_today_calls;
static double   g_today_seconds;
static int      g_today_day;

/* ── Helpers ── */

static void ensure_dir(void)
{
    if (mkdir(g_dir, 0755) != 0 && errno != EEXIST)
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "cannot create directory: %s", g_dir);
}

static FILE *open_daily_csv(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    if (t->tm_yday != g_today_day) {
        g_today_calls = 0;
        g_today_seconds = 0.0;
        g_today_day = t->tm_yday;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%04d-%02d-%02d.csv",
             g_dir, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

    struct stat st;
    int exists = (stat(path, &st) == 0 && st.st_size > 0);

    FILE *fp = fopen(path, "a");
    if (!fp) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "cannot open %s", path);
        return NULL;
    }

    if (!exists) {
        fprintf(fp, "timestamp,date,time,user_id,user_name,method,"
                    "duration_s,emergency,avg_rms,peak_rms,recording\n");
    }

    return fp;
}

static const char *method_str(const char *m)
{
    return (m && m[0]) ? m : "unknown";
}

/* ── Write a CDR record ── */

static void write_cdr(void)
{
    if (!g_in_call) return;

    time_t now = time(NULL);
    double duration = difftime(now, g_call_start);

    struct tm *t = localtime(&now);
    char date[36], tstr[16];
    snprintf(date, sizeof(date), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    snprintf(tstr, sizeof(tstr), "%02d:%02d:%02d",
             t->tm_hour, t->tm_min, t->tm_sec);

    FILE *fp = open_daily_csv();
    if (!fp) return;

    /* Compute average RMS for the call */
    int32_t avg_rms = 0;
    if (g_sq_count > 0) {
        int64_t avg = g_sq_sum / g_sq_count;
        while ((int64_t)avg_rms * avg_rms < avg) avg_rms++;
    }

    fprintf(fp, "%ld,%s,%s,%d,%s,%s,%.1f,%d,%d,%d,%s\n",
            (long)now, date, tstr,
            g_call_user_id,
            g_call_user_name[0] ? g_call_user_name : "unknown",
            method_str(g_call_method),
            duration,
            g_call_emergency,
            (int)avg_rms,
            (int)g_sq_peak_rms,
            g_call_recording);
    fclose(fp);

    g_today_calls++;
    g_today_seconds += duration;

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                "CDR: user=%s method=%s duration=%.1fs avg_rms=%d peak_rms=%d recording=%s",
                g_call_user_name[0] ? g_call_user_name : "unknown",
                method_str(g_call_method), duration,
                (int)avg_rms, (int)g_sq_peak_rms,
                g_call_recording[0] ? g_call_recording : "(none)");
}

/* ── Signal quality audio tap (runs on audio thread) ── */

static void sq_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_in_call || !evt->audio.samples || evt->audio.n == 0)
        return;

    const int16_t *s = evt->audio.samples;
    size_t n = evt->audio.n;

    int64_t frame_sum = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t v = s[i];
        frame_sum += v * v;
    }

    g_sq_sum += frame_sum;
    g_sq_count += (int64_t)n;

    /* Track peak frame RMS */
    int32_t frame_rms = 0;
    int64_t avg = frame_sum / (int64_t)n;
    while ((int64_t)frame_rms * frame_rms < avg) frame_rms++;
    if (frame_rms > g_sq_peak_rms)
        g_sq_peak_rms = frame_rms;
}

/* ── Event handlers ── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;

    g_in_call = 1;
    g_call_start = time(NULL);
    g_call_user_id = 0;
    g_call_user_name[0] = '\0';
    g_call_method[0] = '\0';
    g_call_recording[0] = '\0';
    g_call_emergency = kerchunk_core_get_emergency();
    g_sq_sum = 0;
    g_sq_count = 0;
    g_sq_peak_rms = 0;
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled || !g_in_call) return;

    /*
     * Don't write CDR yet — wait briefly for RECORDING_SAVED event
     * which fires synchronously from mod_recorder's on_cor_drop handler.
     * Since events are dispatched in subscription order and mod_recorder
     * subscribes before mod_cdr (loaded earlier in the module list),
     * RECORDING_SAVED will have already fired by this point and
     * g_call_recording is populated.
     */
    write_cdr();
    g_in_call = 0;
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_in_call) return;

    g_call_user_id = evt->caller.user_id;
    const kerchunk_user_t *u = g_core->user_lookup_by_id(evt->caller.user_id);
    if (u)
        snprintf(g_call_user_name, sizeof(g_call_user_name), "%s", u->name);

    switch (evt->caller.method) {
    case 1: snprintf(g_call_method, sizeof(g_call_method), "CTCSS"); break;
    case 2: snprintf(g_call_method, sizeof(g_call_method), "DCS");   break;
    case 3: snprintf(g_call_method, sizeof(g_call_method), "ANI");   break;
    case 4: snprintf(g_call_method, sizeof(g_call_method), "DTMF");  break;
    case 5: snprintf(g_call_method, sizeof(g_call_method), "WEB");   break;
    default: snprintf(g_call_method, sizeof(g_call_method), "unknown"); break;
    }
}

static void on_recording_saved(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_in_call) return;

    /* Only capture RX recordings (not TX announcements) */
    if (evt->recording.direction &&
        strcmp(evt->recording.direction, "RX") == 0 &&
        evt->recording.path) {
        snprintf(g_call_recording, sizeof(g_call_recording),
                 "%s", evt->recording.path);
    }
}

static void on_announcement(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_enabled) return;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char date[36], tstr[16];
    snprintf(date, sizeof(date), "%04d-%02d-%02d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
    snprintf(tstr, sizeof(tstr), "%02d:%02d:%02d",
             t->tm_hour, t->tm_min, t->tm_sec);

    FILE *fp = open_daily_csv();
    if (!fp) return;

    fprintf(fp, "%ld,%s,%s,0,system,%s,0.0,0,%s\n",
            (long)now, date, tstr,
            evt->announcement.source,
            evt->announcement.description ? evt->announcement.description : "");
    fclose(fp);

    g_today_calls++;
}

/* ── Module lifecycle ── */

static int cdr_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_COR_ASSERT,       on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,          on_cor_drop, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    core->subscribe(KERCHEVT_RECORDING_SAVED,   on_recording_saved, NULL);
    core->subscribe(KERCHEVT_ANNOUNCEMENT,     on_announcement, NULL);
    core->audio_tap_register(sq_audio_tap, NULL);
    return 0;
}

static int cdr_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "cdr", "enabled");
    g_enabled = (!v || strcmp(v, "off") != 0);

    v = kerchunk_config_get(cfg, "cdr", "directory");
    if (v) snprintf(g_dir, sizeof(g_dir), "%s", v);

    if (g_enabled) {
        ensure_dir();
        g_today_day = -1;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "enabled=%d dir=%s", g_enabled, g_dir);
    return 0;
}

static void cdr_unload(void)
{
    if (g_in_call) write_cdr();

    g_core->audio_tap_unregister(sq_audio_tap);
    g_core->unsubscribe(KERCHEVT_COR_ASSERT,       on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,          on_cor_drop);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_RECORDING_SAVED,   on_recording_saved);
    g_core->unsubscribe(KERCHEVT_ANNOUNCEMENT,     on_announcement);
}

/* ── CLI ── */

static int cli_cdr(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    resp_bool(r, "enabled", g_enabled);
    resp_str(r, "directory", g_dir);
    resp_int(r, "today_calls", g_today_calls);
    resp_float(r, "today_seconds", g_today_seconds);
    resp_bool(r, "in_call", g_in_call);
    if (g_in_call) {
        double elapsed = difftime(time(NULL), g_call_start);
        resp_str(r, "active_caller",
                 g_call_user_name[0] ? g_call_user_name : "unknown");
        resp_float(r, "elapsed_s", elapsed);
    }
    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "cdr", "cdr", "CDR status and daily stats", cli_cdr },
};

static kerchunk_module_def_t mod_cdr = {
    .name             = "mod_cdr",
    .version          = "1.0.0",
    .description      = "Call detail records",
    .load             = cdr_load,
    .configure        = cdr_configure,
    .unload           = cdr_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_cdr);
