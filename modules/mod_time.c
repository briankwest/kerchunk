/*
 * mod_time.c — Periodic time announcement
 *
 * Announces current local time on a configurable interval.
 * Skips if repeater is busy (COR active, PTT active, or queue
 * not empty) to avoid stepping on conversations or other
 * automations. DTMF *95# always announces regardless.
 *
 * Config: [time] section in kerchunk.conf
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define LOG_MOD "time"
#define DTMF_EVT_TIME  (KERCHEVT_CUSTOM + 10)

static kerchunk_core_t *g_core;

/* ---- config ---- */
static int  g_enabled      = 0;    /* off by default (FCC 95.1733) */
static int  g_interval_ms  = 900000;   /* 15 min */
static char g_sounds_dir[256] = "/etc/kerchunk/sounds";
static char g_timezone[16] = "";       /* e.g., "central" → tm_central.wav */
static char g_tz_posix[64] = "";       /* POSIX TZ string for localtime_r */
static int  g_timer = -1;

/* Map friendly timezone names to POSIX TZ strings */
static const char *tz_lookup(const char *name)
{
    static const struct { const char *name; const char *tz; } map[] = {
        { "eastern",   "EST5EDT,M3.2.0,M11.1.0" },
        { "central",   "CST6CDT,M3.2.0,M11.1.0" },
        { "mountain",  "MST7MDT,M3.2.0,M11.1.0" },
        { "pacific",   "PST8PDT,M3.2.0,M11.1.0" },
        { "alaska",    "AKST9AKDT,M3.2.0,M11.1.0" },
        { "hawaii",    "HST10" },
        { "arizona",   "MST7" },
        { "utc",       "UTC0" },
        { "gmt",       "GMT0" },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcasecmp(name, map[i].name) == 0)
            return map[i].tz;
    return NULL;  /* not found — caller can use name as raw TZ string */
}

/* Get local time in configured timezone */
static struct tm *local_time_tz(time_t *now)
{
    if (g_tz_posix[0]) {
        setenv("TZ", g_tz_posix, 1);
        tzset();
    }
    return localtime(now);
}

/* ================================================================== */
/*  Speech helpers                                                    */
/* ================================================================== */

static void speak_wav(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.wav", g_sounds_dir, name);
    g_core->queue_audio_file(path, KERCHUNK_PRI_NORMAL);   /* below weather/tones */
}

static void speak_number(int n)
{
    if (n > 199) n = 199;

    if (n >= 100) {
        speak_wav("numbers/num_100");
        n -= 100;
        if (n == 0) return;
    }

    if (n >= 20) {
        char name[32];
        snprintf(name, sizeof(name), "numbers/num_%d", (n / 10) * 10);
        speak_wav(name);
        n %= 10;
        if (n == 0) return;
    } else if (n >= 11) {
        speak_wav("numbers/num_10");
        n -= 10;
    }

    char name[32];
    snprintf(name, sizeof(name), "numbers/num_%d", n);
    speak_wav(name);
}

/* ================================================================== */
/*  Time announcement                                                 */
/* ================================================================== */

static void time_announce(void)
{
    time_t now = time(NULL);
    struct tm *t = local_time_tz(&now);
    int hour = t->tm_hour;
    int min  = t->tm_min;
    int pm   = hour >= 12;

    if (hour > 12) hour -= 12;
    if (hour == 0) hour = 12;

    /* TTS path */
    if (g_core->tts_speak) {
        char text[128];
        if (min == 0)
            snprintf(text, sizeof(text), "The time is %d o'clock %s%s%s.",
                     hour, pm ? "PM" : "AM",
                     g_timezone[0] ? " " : "",
                     g_timezone[0] ? g_timezone : "");
        else
            snprintf(text, sizeof(text), "The time is %d:%02d %s%s%s.",
                     hour, min, pm ? "PM" : "AM",
                     g_timezone[0] ? " " : "",
                     g_timezone[0] ? g_timezone : "");
        g_core->tts_speak(text, KERCHUNK_PRI_NORMAL);
    } else {
        /* WAV fallback */
        speak_wav("time/tm_the_time_is");
        speak_number(hour);
        if (min == 0) {
            speak_wav("time/tm_oclock");
        } else if (min < 10) {
            speak_wav("time/tm_oh");
            speak_number(min);
        } else {
            speak_number(min);
        }
        speak_wav(pm ? "time/tm_pm" : "time/tm_am");
        if (g_timezone[0] != '\0') {
            char tz_wav[48];
            snprintf(tz_wav, sizeof(tz_wav), "time/tm_%s", g_timezone);
            speak_wav(tz_wav);
        }
    }

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "time", .description = "time announcement" } };
    kerchevt_fire(&ae);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "announced %d:%02d %s %s",
                hour, min, pm ? "PM" : "AM",
                g_timezone[0] ? g_timezone : "(no tz)");
}

/* ================================================================== */
/*  Event handlers                                                    */
/* ================================================================== */

static void time_timer_cb(void *ud)
{
    (void)ud;
    if (!g_enabled) return;

    /* Suppress during emergency mode */
    if (kerchunk_core_get_emergency()) return;

    /* Skip if repeater is busy */
    if (g_core->is_receiving() || g_core->is_transmitting() ||
        g_core->queue_depth() > 0) {
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "skipped — repeater busy");
        return;
    }

    time_announce();
}

static void on_dtmf_time(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "time requested via DTMF");
    time_announce();
}

/* ================================================================== */
/*  Module lifecycle                                                  */
/* ================================================================== */

static int time_load(kerchunk_core_t *core)
{
    g_core = core;

    if (core->dtmf_register)
        core->dtmf_register("95", 10, "Time check", "time_check");

    core->subscribe(DTMF_EVT_TIME, on_dtmf_time, NULL);
    return 0;
}

static int time_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "time", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    g_interval_ms = kerchunk_config_get_duration_ms(cfg, "time", "interval", 900000);

    v = kerchunk_config_get(cfg, "time", "timezone");
    if (v) {
        snprintf(g_timezone, sizeof(g_timezone), "%s", v);
        const char *posix = tz_lookup(v);
        if (posix)
            snprintf(g_tz_posix, sizeof(g_tz_posix), "%s", posix);
        else
            snprintf(g_tz_posix, sizeof(g_tz_posix), "%s", v);  /* raw TZ string */

        /* Set process-wide timezone so ALL modules (stats, logger, etc.)
         * use the configured timezone, not system default (UTC). */
        setenv("TZ", g_tz_posix, 1);
        tzset();
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "timezone set: %s (%s)",
                    g_timezone, g_tz_posix);
    }

    v = kerchunk_config_get(cfg, "general", "sounds_dir");
    if (v) snprintf(g_sounds_dir, sizeof(g_sounds_dir), "%s", v);

    if (g_timer >= 0)
        g_core->timer_cancel(g_timer);
    g_timer = -1;
    if (g_enabled && g_interval_ms > 0)
        g_timer = g_core->timer_create(g_interval_ms, 1, time_timer_cb, NULL);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "enabled=%d interval=%dms",
                g_enabled, g_interval_ms);
    return 0;
}

static void time_unload(void)
{
    if (g_core->dtmf_unregister)
        g_core->dtmf_unregister("95");
    g_core->unsubscribe(DTMF_EVT_TIME, on_dtmf_time);
    if (g_timer >= 0)
        g_core->timer_cancel(g_timer);
}

/* ---- CLI ---- */

static int cli_time(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    if (argc >= 2 && strcmp(argv[1], "now") == 0) {
        time_announce();
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "announced");
    } else {
        time_t now = time(NULL);
        struct tm *t = local_time_tz(&now);
        int hour = t->tm_hour;
        int min  = t->tm_min;
        int pm   = hour >= 12;
        if (hour > 12) hour -= 12;
        if (hour == 0) hour = 12;
        resp_bool(r, "enabled", g_enabled);
        resp_int(r, "interval_s", g_interval_ms / 1000);
        char cur[32];
        snprintf(cur, sizeof(cur), "%d:%02d %s", hour, min, pm ? "PM" : "AM");
        resp_str(r, "current", cur);
    }
    return 0;

usage:
    resp_text_raw(r, "Periodic time announcement\n\n"
        "  time\n"
        "    Show time module status: enabled, interval, current time.\n\n"
        "  time now\n"
        "    Immediately announce the current time on-air.\n\n"
        "    Announces local time via TTS or WAV files on a configurable\n"
        "    interval. Skips if repeater is busy or in emergency mode.\n"
        "    DTMF *95# always announces regardless of busy state.\n\n"
        "Config: [time] enabled, interval, timezone\n"
        "DTMF:   *95#\n");
    resp_str(r, "error", "usage: time [now]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "time", .usage = "time [now]", .description = "Time status or announce",
      .handler = cli_time,
      .category = "Announcements", .ui_label = "Time", .ui_type = CLI_UI_BUTTON,
      .ui_command = "time now",
      .subcommands = "now" },
};

static kerchunk_module_def_t mod_time = {
    .name             = "mod_time",
    .version          = "1.0.0",
    .description      = "Time announcements",
    .load             = time_load,
    .configure        = time_configure,
    .unload           = time_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_time);
