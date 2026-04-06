/*
 * mod_logger.c — Event logger
 *
 * Subscribes to all event types and logs them with human-readable
 * detail for debugging and audit.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOG_MOD "logger"

static kerchunk_core_t *g_core;
static FILE *g_log_fp;
static char  g_log_path[256] = "";
static long  g_max_size      = 10 * 1024 * 1024;  /* 10MB default */
static long  g_cur_size;

/* ---- state name helpers ---- */

static const char *state_name(int s)
{
    switch (s) {
    case 0: return "IDLE";
    case 1: return "RECEIVING";
    case 2: return "TAIL_WAIT";
    case 3: return "HANG_WAIT";
    case 4: return "TIMEOUT";
    default: return "???";
    }
}

static const char *caller_method_name(int m)
{
    switch (m) {
    case KERCHUNK_CALLER_DTMF_ANI:   return "ANI";
    case KERCHUNK_CALLER_DTMF_LOGIN: return "LOGIN";
    case KERCHUNK_CALLER_WEB:        return "WEB";
    default:                       return "?";
    }
}

static const char *event_name(kerchevt_type_t type)
{
    switch (type) {
    case KERCHEVT_AUDIO_FRAME:       return "AUDIO_FRAME";
    case KERCHEVT_CTCSS_DETECT:      return "CTCSS_DETECT";
    case KERCHEVT_DCS_DETECT:        return "DCS_DETECT";
    case KERCHEVT_DTMF_DIGIT:        return "DTMF_DIGIT";
    case KERCHEVT_DTMF_END:          return "DTMF_END";
    case KERCHEVT_COR_ASSERT:        return "COR_ASSERT";
    case KERCHEVT_COR_DROP:          return "COR_DROP";
    case KERCHEVT_PTT_ASSERT:        return "PTT_ASSERT";
    case KERCHEVT_PTT_DROP:          return "PTT_DROP";
    case KERCHEVT_STATE_CHANGE:      return "STATE_CHANGE";
    case KERCHEVT_TAIL_START:        return "TAIL_START";
    case KERCHEVT_TAIL_EXPIRE:       return "TAIL_EXPIRE";
    case KERCHEVT_TIMEOUT:           return "TIMEOUT";
    case KERCHEVT_CALLER_IDENTIFIED: return "CALLER_IDENTIFIED";
    case KERCHEVT_CALLER_CLEARED:    return "CALLER_CLEARED";
    case KERCHEVT_QUEUE_DRAIN:       return "QUEUE_DRAIN";
    case KERCHEVT_QUEUE_COMPLETE:    return "QUEUE_COMPLETE";
    case KERCHEVT_QUEUE_PREEMPTED:  return "QUEUE_PREEMPTED";
    case KERCHEVT_ANNOUNCEMENT:      return "ANNOUNCEMENT";
    case KERCHEVT_CONFIG_RELOAD:     return "CONFIG_RELOAD";
    case KERCHEVT_SHUTDOWN:          return "SHUTDOWN";
    case KERCHEVT_TICK:              return "TICK";
    default:
        if ((int)type >= KERCHEVT_CUSTOM) {
            static char buf[32];
            snprintf(buf, sizeof(buf), "CUSTOM+%d", (int)type - KERCHEVT_CUSTOM);
            return buf;
        }
        return "UNKNOWN";
    }
}

/* Map custom event offsets to names */
static const char *custom_event_name(int offset)
{
    switch (offset) {
    case 0:  return "VM_STATUS";
    case 1:  return "VM_RECORD";
    case 2:  return "VM_PLAY";
    case 3:  return "VM_DELETE";
    case 4:  return "VM_LIST";
    case 5:  return "GPIO_ON";
    case 6:  return "GPIO_OFF";
    case 7:  return "CALLER_ID";
    case 8:  return "WEATHER";
    case 9:  return "FORECAST";
    case 10: return "TIME";
    case 11: return "EMERGENCY_ON";
    case 12: return "EMERGENCY_OFF";
    case 13: return "PARROT";
    case 14: return "NWS";
    case 15: return "OTP_AUTH";
    default: return NULL;
    }
}

/* Format timestamp as HH:MM:SS.mmm */
static void fmt_time(char *buf, size_t max)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm *tm = localtime(&ts.tv_sec);
    snprintf(buf, max, "%02d:%02d:%02d.%03ld",
             tm->tm_hour, tm->tm_min, tm->tm_sec,
             ts.tv_nsec / 1000000);
}

static void log_event(const kerchevt_t *evt, void *ud)
{
    (void)ud;

    /* Skip high-frequency events */
    if (evt->type == KERCHEVT_AUDIO_FRAME || evt->type == KERCHEVT_TICK)
        return;

    char detail[256] = "";

    switch (evt->type) {
    case KERCHEVT_CTCSS_DETECT:
        snprintf(detail, sizeof(detail), " tone=%u.%u Hz %s",
                 evt->ctcss.freq_x10 / 10, evt->ctcss.freq_x10 % 10,
                 evt->ctcss.active ? "detected" : "lost");
        break;

    case KERCHEVT_DCS_DETECT:
        snprintf(detail, sizeof(detail), " code=%03u %s %s",
                 evt->dcs.code,
                 evt->dcs.normal ? "normal" : "inverted",
                 evt->dcs.active ? "detected" : "lost");
        break;

    case KERCHEVT_DTMF_DIGIT:
        snprintf(detail, sizeof(detail), " digit='%c' dur=%dms",
                 evt->dtmf.digit, evt->dtmf.duration_ms);
        break;

    case KERCHEVT_DTMF_END:
        snprintf(detail, sizeof(detail), " end");
        break;

    case KERCHEVT_COR_ASSERT:
        snprintf(detail, sizeof(detail), " carrier detected");
        break;

    case KERCHEVT_COR_DROP:
        snprintf(detail, sizeof(detail), " carrier lost");
        break;

    case KERCHEVT_PTT_ASSERT:
        snprintf(detail, sizeof(detail), " transmitter keyed (queue=%d)",
                 g_core->queue_depth());
        break;

    case KERCHEVT_PTT_DROP:
        snprintf(detail, sizeof(detail), " transmitter unkeyed");
        break;

    case KERCHEVT_STATE_CHANGE:
        snprintf(detail, sizeof(detail), " %s → %s",
                 state_name(evt->state.old_state),
                 state_name(evt->state.new_state));
        break;

    case KERCHEVT_CALLER_IDENTIFIED: {
        const kerchunk_user_t *u = g_core->user_lookup_by_id(evt->caller.user_id);
        snprintf(detail, sizeof(detail), " user=%d (%s) via %s",
                 evt->caller.user_id,
                 u ? u->name : "unknown",
                 caller_method_name(evt->caller.method));
        break;
    }

    case KERCHEVT_CALLER_CLEARED:
        snprintf(detail, sizeof(detail), " caller cleared");
        break;

    case KERCHEVT_QUEUE_DRAIN:
        snprintf(detail, sizeof(detail), " item=%d", evt->queue.item_id);
        break;

    case KERCHEVT_QUEUE_COMPLETE:
        snprintf(detail, sizeof(detail), " item=%d done", evt->queue.item_id);
        break;

    case KERCHEVT_QUEUE_PREEMPTED:
        snprintf(detail, sizeof(detail), " source=%s flushed=%d",
                 evt->preempt.source ? evt->preempt.source : "?",
                 evt->preempt.items_flushed);
        break;

    case KERCHEVT_TIMEOUT:
        snprintf(detail, sizeof(detail), " TOT fired — user timed out");
        break;

    case KERCHEVT_TAIL_START:
        snprintf(detail, sizeof(detail), " tail timer started");
        break;

    case KERCHEVT_TAIL_EXPIRE:
        snprintf(detail, sizeof(detail), " tail timer expired");
        break;

    case KERCHEVT_SHUTDOWN:
        snprintf(detail, sizeof(detail), " daemon shutting down");
        break;

    case KERCHEVT_ANNOUNCEMENT:
        snprintf(detail, sizeof(detail), " source=%s desc=%s",
                 evt->announcement.source ? evt->announcement.source : "?",
                 evt->announcement.description ? evt->announcement.description : "");
        break;

    case KERCHEVT_CONFIG_RELOAD:
        snprintf(detail, sizeof(detail), " configuration reloaded");
        break;

    default:
        /* Custom events — show name and data if present */
        if ((int)evt->type >= KERCHEVT_CUSTOM) {
            int offset = (int)evt->type - KERCHEVT_CUSTOM;
            const char *name = custom_event_name(offset);
            if (evt->custom.data && evt->custom.len > 0)
                snprintf(detail, sizeof(detail), " %s arg=\"%.*s\"",
                         name ? name : "?",
                         (int)evt->custom.len, (const char *)evt->custom.data);
            else if (name)
                snprintf(detail, sizeof(detail), " %s", name);
        }
        break;
    }

    /* Human-readable timestamp */
    char ts[52];
    fmt_time(ts, sizeof(ts));

    const char *ename = event_name(evt->type);

    /* Write event to log file (raw event format, not via tee) */
    if (g_log_fp) {
        int n = fprintf(g_log_fp, "%s %-20s%s\n", ts, ename, detail);
        fflush(g_log_fp);

        /* Log rotation: rename current → .1 and open fresh */
        if (n > 0) g_cur_size += n;
        if (g_max_size > 0 && g_cur_size >= g_max_size && g_log_path[0]) {
            char old_path[260];
            snprintf(old_path, sizeof(old_path), "%s.1", g_log_path);
            fclose(g_log_fp);
            kerchunk_log_tee_remove();
            rename(g_log_path, old_path);
            g_log_fp = fopen(g_log_path, "a");
            if (g_log_fp) {
                kerchunk_log_tee_file(g_log_fp);
                g_cur_size = 0;
            }
        }
    }
}

static int logger_load(kerchunk_core_t *core)
{
    g_core = core;

    /* Subscribe to all non-high-frequency event types */
    kerchevt_type_t types[] = {
        KERCHEVT_CTCSS_DETECT, KERCHEVT_DCS_DETECT, KERCHEVT_DTMF_DIGIT, KERCHEVT_DTMF_END,
        KERCHEVT_COR_ASSERT, KERCHEVT_COR_DROP, KERCHEVT_PTT_ASSERT, KERCHEVT_PTT_DROP,
        KERCHEVT_STATE_CHANGE, KERCHEVT_TAIL_START, KERCHEVT_TAIL_EXPIRE, KERCHEVT_TIMEOUT,
        KERCHEVT_CALLER_IDENTIFIED, KERCHEVT_CALLER_CLEARED,
        KERCHEVT_QUEUE_DRAIN, KERCHEVT_QUEUE_COMPLETE, KERCHEVT_QUEUE_PREEMPTED,
        KERCHEVT_ANNOUNCEMENT,
        KERCHEVT_CONFIG_RELOAD, KERCHEVT_SHUTDOWN,
    };
    for (int i = 0; i < (int)(sizeof(types) / sizeof(types[0])); i++)
        core->subscribe(types[i], log_event, NULL);

    /* Subscribe to known custom events */
    for (int i = 0; i <= 15; i++)
        core->subscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i), log_event, NULL);

    return 0;
}

static int logger_configure(const kerchunk_config_t *cfg)
{
    g_max_size = (long)kerchunk_config_get_int(cfg, "logger", "max_size_mb", 10)
                 * 1024L * 1024L;

    const char *path = kerchunk_config_get(cfg, "logger", "file");
    if (path) {
        if (g_log_fp) {
            fclose(g_log_fp);
            g_log_fp = NULL;
        }
        snprintf(g_log_path, sizeof(g_log_path), "%s", path);
        g_log_fp = fopen(path, "a");
        if (!g_log_fp) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to open log: %s", path);
        } else {
            kerchunk_log_tee_file(g_log_fp);
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "logging to: %s", path);
        }
    }
    return 0;
}

static const kerchevt_type_t g_sub_types[] = {
    KERCHEVT_CTCSS_DETECT, KERCHEVT_DCS_DETECT, KERCHEVT_DTMF_DIGIT, KERCHEVT_DTMF_END,
    KERCHEVT_COR_ASSERT, KERCHEVT_COR_DROP, KERCHEVT_PTT_ASSERT, KERCHEVT_PTT_DROP,
    KERCHEVT_STATE_CHANGE, KERCHEVT_TAIL_START, KERCHEVT_TAIL_EXPIRE, KERCHEVT_TIMEOUT,
    KERCHEVT_CALLER_IDENTIFIED, KERCHEVT_CALLER_CLEARED,
    KERCHEVT_QUEUE_DRAIN, KERCHEVT_QUEUE_COMPLETE, KERCHEVT_QUEUE_PREEMPTED,
    KERCHEVT_CONFIG_RELOAD, KERCHEVT_SHUTDOWN,
};
#define NUM_SUB_TYPES (int)(sizeof(g_sub_types) / sizeof(g_sub_types[0]))

static void logger_unload(void)
{
    for (int i = 0; i < NUM_SUB_TYPES; i++)
        g_core->unsubscribe(g_sub_types[i], log_event);
    for (int i = 0; i <= 15; i++)
        g_core->unsubscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i), log_event);

    kerchunk_log_tee_remove();
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

static kerchunk_module_def_t mod_logger = {
    .name        = "mod_logger",
    .version     = "1.0.0",
    .description = "Event logger",
    .load        = logger_load,
    .configure   = logger_configure,
    .unload      = logger_unload,
};

KERCHUNK_MODULE_DEFINE(mod_logger);
