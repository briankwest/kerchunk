/*
 * kerchunk_events.h — Event bus types and API
 */

#ifndef KERCHUNK_EVENTS_H
#define KERCHUNK_EVENTS_H

#include <stdint.h>
#include <stddef.h>

/* Event types */
typedef enum {
    /* Audio/detection events */
    KERCHEVT_AUDIO_FRAME,
    KERCHEVT_CTCSS_DETECT,
    KERCHEVT_DCS_DETECT,
    KERCHEVT_DTMF_DIGIT,
    KERCHEVT_DTMF_END,

    /* COR/PTT events */
    KERCHEVT_COR_ASSERT,
    KERCHEVT_COR_DROP,
    KERCHEVT_VCOR_ASSERT,       /* Virtual COR — web PTT, PoC, phone */
    KERCHEVT_VCOR_DROP,
    KERCHEVT_PTT_ASSERT,
    KERCHEVT_PTT_DROP,
    KERCHEVT_LINK_RX_ASSERT,    /* mod_link is now playing remote audio
                                 * out the local TX (BRIDGING). Distinct
                                 * from COR_ASSERT so subscribers like
                                 * mod_recorder/mod_cdr don't duplicate
                                 * the reflector's server-side recording. */
    KERCHEVT_LINK_RX_DROP,

    /* Repeater state events */
    KERCHEVT_RX_STATE_CHANGE,   /* mod_repeater FSM (IDLE / RECEIVING
                                 * / TAIL_WAIT / HANG_WAIT / RX_TIMEOUT).
                                 * JSON wire: "rx_state_change". */
    KERCHEVT_TX_STATE_CHANGE,   /* audio-thread FSM (IDLE / RELAY /
                                 * DELAY / DRAIN / TAIL / HOLD).
                                 * JSON wire: "tx_state_change".
                                 * Payload uses the state union below
                                 * with old_state/new_state populated
                                 * from kerchunk_tx_state_t. */
    KERCHEVT_TAIL_START,
    KERCHEVT_TAIL_EXPIRE,
    KERCHEVT_RX_TIMEOUT,        /* TOT fire (mod_repeater).
                                 * JSON wire: "rx_timeout". */

    /* Caller events */
    KERCHEVT_CALLER_IDENTIFIED,
    KERCHEVT_CALLER_CLEARED,

    /* Module/system events */
    KERCHEVT_QUEUE_DRAIN,
    KERCHEVT_QUEUE_COMPLETE,
    KERCHEVT_QUEUE_PREEMPTED,
    KERCHEVT_RECORDING_SAVED,
    KERCHEVT_ANNOUNCEMENT,
    KERCHEVT_CONFIG_RELOAD,
    KERCHEVT_SHUTDOWN,
    KERCHEVT_TICK,
    KERCHEVT_HEARTBEAT,

    /* Sentinel for built-in count */
    KERCHEVT_BUILTIN_COUNT,

    /* Custom events (modules define 1000+) */
    KERCHEVT_CUSTOM = 1000,
} kerchevt_type_t;

/* Event structure */
typedef struct {
    kerchevt_type_t type;
    uint64_t      timestamp_us;
    union {
        struct { int16_t *samples; size_t n; }             audio;
        struct { uint16_t freq_x10; int active; }          ctcss;
        struct { uint16_t code; int normal; int active; }  dcs;
        struct { char digit; int duration_ms; }            dtmf;
        struct { int active; }                             cor;
        struct { const char *source; int user_id; }       vcor;
        struct { int old_state; int new_state; }           state;
        struct { int user_id; int method; }                caller;
        struct { int item_id; uint32_t duration_ms; }        queue;
        struct { const char *path; const char *direction;
                 int user_id; float duration; }            recording;
        struct { const char *source; const char *description; } announcement;
        struct { const char *source; int items_flushed; }     preempt;
        struct { void *data; size_t len; }                 custom;
    };
} kerchevt_t;

/* Subscription callback */
typedef void (*kerchevt_handler_t)(const kerchevt_t *evt, void *userdata);

/* Maximum subscribers per event type */
#define KERCHEVT_MAX_SUBS 32

/* Maximum total event types (built-in + custom) */
#define KERCHEVT_MAX_TYPES 1128

/* Event bus API */
int  kerchevt_init(void);
void kerchevt_shutdown(void);
int  kerchevt_subscribe(kerchevt_type_t type, kerchevt_handler_t handler, void *userdata);
int  kerchevt_unsubscribe(kerchevt_type_t type, kerchevt_handler_t handler);
void kerchevt_fire(const kerchevt_t *evt);
int  kerchevt_subscriber_count(kerchevt_type_t type);

/* Canonical RX-state label for the integer values mod_repeater puts
 * into evt->state.{old,new}_state. Single source of truth for
 * mod_repeater, mod_logger, and kerchunk_evt_json. Returns "UNKNOWN"
 * for out-of-range inputs. Mirrors kerchunk_tx_state_name() for the
 * TX FSM. */
const char *kerchunk_rx_state_name(int s);

/* Canonical event-type vocabulary. Two views over one static
 * table — the wire-format string (lowercase, e.g. "rx_state_change")
 * and a reverse name→type lookup. Replaces parallel switch
 * statements in mod_logger / mod_webhook / kerchunk_evt_json.
 *
 *   kerchunk_event_name(t)       — wire string, "" if unknown.
 *   kerchunk_event_from_name(s)  — type, returns -1 on miss.
 *
 * Custom events (KERCHEVT_CUSTOM + N) report as "custom" / "custom_N".
 */
const char     *kerchunk_event_name(kerchevt_type_t type);
kerchevt_type_t kerchunk_event_from_name(const char *name);

#endif /* KERCHUNK_EVENTS_H */
