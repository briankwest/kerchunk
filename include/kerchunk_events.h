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

    /* Repeater state events */
    KERCHEVT_STATE_CHANGE,      /* RX state (mod_repeater).
                                 * Renamed to KERCHEVT_RX_STATE_CHANGE
                                 * in Phase 2 — still named STATE_CHANGE
                                 * here during Phase 1 to avoid touching
                                 * every subscriber in this commit. */
    KERCHEVT_TX_STATE_CHANGE,   /* TX state (audio thread).
                                 * Payload uses the state union below,
                                 * with old_state/new_state populated
                                 * from kerchunk_tx_state_t. */
    KERCHEVT_TAIL_START,
    KERCHEVT_TAIL_EXPIRE,
    KERCHEVT_TIMEOUT,

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

#endif /* KERCHUNK_EVENTS_H */
