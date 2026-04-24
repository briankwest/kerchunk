/*
 * kerchunk_events.c — Thread-safe event bus
 *
 * Synchronous dispatch with a mutex so both the audio thread
 * and the main thread can fire/subscribe safely.
 */

#include "kerchunk_events.h"
#include "kerchunk_log.h"
#include <string.h>
#include <pthread.h>
#include <time.h>

#define LOG_MOD "events"

typedef struct {
    kerchevt_handler_t handler;
    void            *userdata;
} sub_slot_t;

typedef struct {
    sub_slot_t subs[KERCHEVT_MAX_SUBS];
    int        count;
} sub_list_t;

static sub_list_t     g_subs[KERCHEVT_MAX_TYPES];
static int            g_initialized;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* RX FSM state-name lookup.  The integer values match the
 * mod_repeater enum (RPT_IDLE..RPT_RX_TIMEOUT). Kept here so it
 * can be exported from the daemon for both mod_logger (a .so) and
 * kerchunk_evt_json (linked into the daemon binary). */
const char *kerchunk_rx_state_name(int s)
{
    switch (s) {
    case 0: return "IDLE";
    case 1: return "RECEIVING";
    case 2: return "TAIL_WAIT";
    case 3: return "HANG_WAIT";
    case 4: return "RX_TIMEOUT";
    default: return "UNKNOWN";
    }
}

/* Single source of truth for the event-type ↔ wire-name vocabulary.
 * Adding a new built-in event type now means one row here instead of
 * three switch statements. KERCHEVT_TICK / KERCHEVT_AUDIO_FRAME stay
 * in the table for completeness even though they aren't typically
 * serialized (high-frequency, dropped by SSE/log/webhook filters). */
typedef struct {
    kerchevt_type_t type;
    const char     *name;
} evt_name_row_t;

static const evt_name_row_t g_evt_names[] = {
    { KERCHEVT_AUDIO_FRAME,       "audio_frame"       },
    { KERCHEVT_CTCSS_DETECT,      "ctcss_detect"      },
    { KERCHEVT_DCS_DETECT,        "dcs_detect"        },
    { KERCHEVT_DTMF_DIGIT,        "dtmf_digit"        },
    { KERCHEVT_DTMF_END,          "dtmf_end"          },
    { KERCHEVT_COR_ASSERT,        "cor_assert"        },
    { KERCHEVT_COR_DROP,          "cor_drop"          },
    { KERCHEVT_VCOR_ASSERT,       "vcor_assert"       },
    { KERCHEVT_VCOR_DROP,         "vcor_drop"         },
    { KERCHEVT_PTT_ASSERT,        "ptt_assert"        },
    { KERCHEVT_PTT_DROP,          "ptt_drop"          },
    { KERCHEVT_RX_STATE_CHANGE,   "rx_state_change"   },
    { KERCHEVT_TX_STATE_CHANGE,   "tx_state_change"   },
    { KERCHEVT_TAIL_START,        "tail_start"        },
    { KERCHEVT_TAIL_EXPIRE,       "tail_expire"       },
    { KERCHEVT_RX_TIMEOUT,        "rx_timeout"        },
    { KERCHEVT_CALLER_IDENTIFIED, "caller_identified" },
    { KERCHEVT_CALLER_CLEARED,    "caller_cleared"    },
    { KERCHEVT_QUEUE_DRAIN,       "queue_drain"       },
    { KERCHEVT_QUEUE_COMPLETE,    "queue_complete"    },
    { KERCHEVT_QUEUE_PREEMPTED,   "queue_preempted"   },
    { KERCHEVT_RECORDING_SAVED,   "recording_saved"   },
    { KERCHEVT_ANNOUNCEMENT,      "announcement"      },
    { KERCHEVT_CONFIG_RELOAD,     "config_reload"     },
    { KERCHEVT_SHUTDOWN,          "shutdown"          },
    { KERCHEVT_TICK,              "tick"              },
    { KERCHEVT_HEARTBEAT,         "heartbeat"         },
};
#define EVT_NAMES_COUNT ((int)(sizeof(g_evt_names) / sizeof(g_evt_names[0])))

const char *kerchunk_event_name(kerchevt_type_t t)
{
    for (int i = 0; i < EVT_NAMES_COUNT; i++)
        if (g_evt_names[i].type == t)
            return g_evt_names[i].name;
    if ((int)t >= KERCHEVT_CUSTOM)
        return "custom";
    return "";
}

kerchevt_type_t kerchunk_event_from_name(const char *name)
{
    if (!name) return (kerchevt_type_t)-1;
    for (int i = 0; i < EVT_NAMES_COUNT; i++)
        if (strcmp(g_evt_names[i].name, name) == 0)
            return g_evt_names[i].type;
    return (kerchevt_type_t)-1;
}

/* Per-thread event dispatch depth. Synchronous event dispatch means a
 * handler can fire another event, which dispatches more handlers on the
 * same thread's stack. Without a ceiling, a buggy fire→handler→fire
 * chain runs until the stack is exhausted — which looks like a system
 * hang, not a crash, because the thread is busy recursing.
 *
 * We cap at KERCHEVT_MAX_DEPTH. Over the limit, we log and drop the fire.
 * This breaks the loop and leaves a loud breadcrumb in the log so the
 * actual cycle can be identified. */
#define KERCHEVT_MAX_DEPTH 16
static __thread int   g_fire_depth;

static int evt_index(kerchevt_type_t type)
{
    if (type < KERCHEVT_BUILTIN_COUNT)
        return (int)type;
    if (type >= KERCHEVT_CUSTOM && type < KERCHEVT_CUSTOM + (KERCHEVT_MAX_TYPES - KERCHEVT_BUILTIN_COUNT))
        return KERCHEVT_BUILTIN_COUNT + (type - KERCHEVT_CUSTOM);
    return -1;
}

int kerchevt_init(void)
{
    pthread_mutex_lock(&g_mutex);
    memset(g_subs, 0, sizeof(g_subs));
    g_initialized = 1;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void kerchevt_shutdown(void)
{
    pthread_mutex_lock(&g_mutex);
    memset(g_subs, 0, sizeof(g_subs));
    g_initialized = 0;
    pthread_mutex_unlock(&g_mutex);
}

int kerchevt_subscribe(kerchevt_type_t type, kerchevt_handler_t handler, void *userdata)
{
    if (!handler) return -1;

    pthread_mutex_lock(&g_mutex);
    if (!g_initialized) { pthread_mutex_unlock(&g_mutex); return -1; }

    int idx = evt_index(type);
    if (idx < 0) { pthread_mutex_unlock(&g_mutex); return -1; }

    sub_list_t *sl = &g_subs[idx];

    /* Reject duplicate: same handler already subscribed to this event */
    for (int i = 0; i < sl->count; i++) {
        if (sl->subs[i].handler == handler) {
            pthread_mutex_unlock(&g_mutex);
            return 0;  /* Already subscribed — not an error */
        }
    }

    if (sl->count >= KERCHEVT_MAX_SUBS) {
        pthread_mutex_unlock(&g_mutex);
        KERCHUNK_LOG_E(LOG_MOD, "max subscribers for event %d", type);
        return -1;
    }

    sl->subs[sl->count].handler  = handler;
    sl->subs[sl->count].userdata = userdata;
    sl->count++;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

int kerchevt_unsubscribe(kerchevt_type_t type, kerchevt_handler_t handler)
{
    if (!handler) return -1;

    pthread_mutex_lock(&g_mutex);
    if (!g_initialized) { pthread_mutex_unlock(&g_mutex); return -1; }

    int idx = evt_index(type);
    if (idx < 0) { pthread_mutex_unlock(&g_mutex); return -1; }

    sub_list_t *sl = &g_subs[idx];
    for (int i = 0; i < sl->count; i++) {
        if (sl->subs[i].handler == handler) {
            for (int j = i; j < sl->count - 1; j++)
                sl->subs[j] = sl->subs[j + 1];
            sl->count--;
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mutex);
    return -1;
}

void kerchevt_fire(const kerchevt_t *evt)
{
    if (!evt) return;

    /* Auto-stamp events that don't have a timestamp */
    if (evt->timestamp_us == 0) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        /* Cast away const for timestamp only — callers can skip setting it */
        ((kerchevt_t *)evt)->timestamp_us =
            (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
    }

    /* Recursion ceiling: break cycles before they hang the thread */
    if (g_fire_depth >= KERCHEVT_MAX_DEPTH) {
        KERCHUNK_LOG_E(LOG_MOD,
            "event recursion limit (%d) hit firing type=%d — dropping to break cycle",
            KERCHEVT_MAX_DEPTH, (int)evt->type);
        return;
    }

    pthread_mutex_lock(&g_mutex);
    if (!g_initialized) { pthread_mutex_unlock(&g_mutex); return; }

    int idx = evt_index(evt->type);
    if (idx < 0) { pthread_mutex_unlock(&g_mutex); return; }

    /* Snapshot under lock, dispatch after unlock to avoid deadlock
     * if a handler fires another event */
    sub_list_t *sl = &g_subs[idx];
    int count = sl->count;
    sub_slot_t snapshot[KERCHEVT_MAX_SUBS];
    for (int i = 0; i < count; i++)
        snapshot[i] = sl->subs[i];
    pthread_mutex_unlock(&g_mutex);

    g_fire_depth++;
    for (int i = 0; i < count; i++)
        snapshot[i].handler(evt, snapshot[i].userdata);
    g_fire_depth--;
}

int kerchevt_subscriber_count(kerchevt_type_t type)
{
    pthread_mutex_lock(&g_mutex);
    if (!g_initialized) { pthread_mutex_unlock(&g_mutex); return 0; }
    int idx = evt_index(type);
    int n = (idx >= 0) ? g_subs[idx].count : 0;
    pthread_mutex_unlock(&g_mutex);
    return n;
}
