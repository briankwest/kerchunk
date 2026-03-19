/*
 * kerchunk_evt_json.c — Event-to-JSON serializer
 *
 * Converts kerchevt_t events to typed JSON objects for the event stream.
 * Used by the socket layer for __JSON_EVENTS__ subscribers and by
 * mod_web for SSE.
 */

#include "kerchunk.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>

static const char *state_name(int s)
{
    switch (s) {
    case 0: return "IDLE";
    case 1: return "RECEIVING";
    case 2: return "TAIL_WAIT";
    case 3: return "HANG_WAIT";
    case 4: return "TIMEOUT";
    default: return "UNKNOWN";
    }
}

static const char *method_name(int m)
{
    switch (m) {
    case 1: return "CTCSS";
    case 2: return "DCS";
    case 3: return "ANI";
    case 4: return "DTMF";
    default: return "unknown";
    }
}

int kerchevt_to_json(const kerchevt_t *evt, char *buf, size_t max)
{
    unsigned long long ts = (unsigned long long)evt->timestamp_us;

    switch (evt->type) {
    case KERCHEVT_COR_ASSERT:
        return snprintf(buf, max,
            "{\"type\":\"cor_assert\",\"ts\":%llu}", ts);

    case KERCHEVT_COR_DROP:
        return snprintf(buf, max,
            "{\"type\":\"cor_drop\",\"ts\":%llu}", ts);

    case KERCHEVT_PTT_ASSERT:
        return snprintf(buf, max,
            "{\"type\":\"ptt_assert\",\"ts\":%llu}", ts);

    case KERCHEVT_PTT_DROP:
        return snprintf(buf, max,
            "{\"type\":\"ptt_drop\",\"ts\":%llu}", ts);

    case KERCHEVT_STATE_CHANGE:
        return snprintf(buf, max,
            "{\"type\":\"state_change\",\"old_state\":\"%s\","
            "\"new_state\":\"%s\",\"ts\":%llu}",
            state_name(evt->state.old_state),
            state_name(evt->state.new_state), ts);

    case KERCHEVT_TAIL_START:
        return snprintf(buf, max,
            "{\"type\":\"tail_start\",\"ts\":%llu}", ts);

    case KERCHEVT_TAIL_EXPIRE:
        return snprintf(buf, max,
            "{\"type\":\"tail_expire\",\"ts\":%llu}", ts);

    case KERCHEVT_TIMEOUT:
        return snprintf(buf, max,
            "{\"type\":\"timeout\",\"ts\":%llu}", ts);

    case KERCHEVT_CALLER_IDENTIFIED:
        return snprintf(buf, max,
            "{\"type\":\"caller_identified\",\"user_id\":%d,"
            "\"method\":\"%s\",\"ts\":%llu}",
            evt->caller.user_id,
            method_name(evt->caller.method), ts);

    case KERCHEVT_CALLER_CLEARED:
        return snprintf(buf, max,
            "{\"type\":\"caller_cleared\",\"ts\":%llu}", ts);

    case KERCHEVT_CTCSS_DETECT:
        return snprintf(buf, max,
            "{\"type\":\"ctcss_detect\",\"freq_hz\":%.1f,"
            "\"active\":%s,\"ts\":%llu}",
            evt->ctcss.freq_x10 / 10.0,
            evt->ctcss.active ? "true" : "false", ts);

    case KERCHEVT_DCS_DETECT:
        return snprintf(buf, max,
            "{\"type\":\"dcs_detect\",\"code\":%d,"
            "\"active\":%s,\"ts\":%llu}",
            evt->dcs.code,
            evt->dcs.active ? "true" : "false", ts);

    case KERCHEVT_DTMF_DIGIT:
        return snprintf(buf, max,
            "{\"type\":\"dtmf_digit\",\"digit\":\"%c\",\"ts\":%llu}",
            evt->dtmf.digit, ts);

    case KERCHEVT_DTMF_END:
        return snprintf(buf, max,
            "{\"type\":\"dtmf_end\",\"ts\":%llu}", ts);

    case KERCHEVT_QUEUE_DRAIN:
        return snprintf(buf, max,
            "{\"type\":\"queue_drain\",\"ts\":%llu}", ts);

    case KERCHEVT_QUEUE_COMPLETE:
        return snprintf(buf, max,
            "{\"type\":\"queue_complete\",\"ts\":%llu}", ts);

    case KERCHEVT_RECORDING_SAVED:
        return snprintf(buf, max,
            "{\"type\":\"recording_saved\",\"direction\":\"%s\","
            "\"path\":\"%s\",\"duration\":%.1f,\"user_id\":%d,\"ts\":%llu}",
            evt->recording.direction ? evt->recording.direction : "",
            evt->recording.path ? evt->recording.path : "",
            evt->recording.duration,
            evt->recording.user_id, ts);

    case KERCHEVT_ANNOUNCEMENT:
        return snprintf(buf, max,
            "{\"type\":\"announcement\",\"source\":\"%s\","
            "\"description\":\"%s\",\"ts\":%llu}",
            evt->announcement.source ? evt->announcement.source : "",
            evt->announcement.description ? evt->announcement.description : "",
            ts);

    case KERCHEVT_CONFIG_RELOAD:
        return snprintf(buf, max,
            "{\"type\":\"config_reload\",\"ts\":%llu}", ts);

    case KERCHEVT_SHUTDOWN:
        return snprintf(buf, max,
            "{\"type\":\"shutdown\",\"ts\":%llu}", ts);

    default:
        /* Custom events or unknown */
        if (evt->type >= KERCHEVT_CUSTOM)
            return snprintf(buf, max,
                "{\"type\":\"custom\",\"id\":%d,\"ts\":%llu}",
                evt->type - KERCHEVT_CUSTOM, ts);
        return snprintf(buf, max,
            "{\"type\":\"unknown\",\"code\":%d,\"ts\":%llu}",
            evt->type, ts);
    }
}
