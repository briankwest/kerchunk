/*
 * kerchunk.h — Master header for kerchunkd repeater controller
 *
 * Includes all public types and the core API that modules receive.
 * C99, depends only on libc + libm + libplcode.
 */

#ifndef KERCHUNK_H
#define KERCHUNK_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ── Response object — defined before module.h needs it ── */

#define RESP_MAX 4096

typedef struct kerchunk_resp {
    char text[RESP_MAX];
    int  tlen;
    char json[RESP_MAX];
    int  jlen;
    int  jfirst;
} kerchunk_resp_t;

void resp_init(kerchunk_resp_t *r);
void resp_str(kerchunk_resp_t *r, const char *key, const char *val);
void resp_int(kerchunk_resp_t *r, const char *key, int val);
void resp_int64(kerchunk_resp_t *r, const char *key, int64_t val);
void resp_bool(kerchunk_resp_t *r, const char *key, int val);
void resp_float(kerchunk_resp_t *r, const char *key, double val);
void resp_json_raw(kerchunk_resp_t *r, const char *fragment);
void resp_text_raw(kerchunk_resp_t *r, const char *fragment);
void resp_finish(kerchunk_resp_t *r);

#include "kerchunk_events.h"
#include "kerchunk_module.h"
#include "kerchunk_queue.h"
#include "kerchunk_audio.h"
#include "kerchunk_hid.h"
#include "kerchunk_config.h"
#include "kerchunk_log.h"
#include "kerchunk_user.h"
#include "kerchunk_wav.h"
#include "kerchunk_sched.h"

/* Version */
#define KERCHUNK_VERSION_MAJOR 1
#define KERCHUNK_VERSION_MINOR 0
#define KERCHUNK_VERSION_PATCH 2

#ifdef KERCHUNK_GIT_HASH
#define KERCHUNK_VERSION_STRING "1.0.2+" KERCHUNK_GIT_HASH
#else
#define KERCHUNK_VERSION_STRING "1.0.2"
#endif

/* Frame timing: 20ms frames, max rate 48 kHz → max 960 samples/frame */
#define KERCHUNK_FRAME_MS          20
#define KERCHUNK_MAX_SAMPLE_RATE   48000
#define KERCHUNK_MAX_FRAME_SAMPLES ((KERCHUNK_MAX_SAMPLE_RATE * KERCHUNK_FRAME_MS) / 1000)

/* Queue priority levels (higher = plays sooner when queue is idle) */
#define KERCHUNK_PRI_LOW          1   /* CW ID, voicemail, GPIO confirms */
#define KERCHUNK_PRI_NORMAL       2   /* Time, parrot, courtesy tones, web PTT */
#define KERCHUNK_PRI_ELEVATED     3   /* Weather, OTP, TTS announcements */
#define KERCHUNK_PRI_HIGH         4   /* Repeater system tones, NWS errors */
#define KERCHUNK_PRI_CRITICAL    10   /* Emergency, timeout warning */

/* Core API passed to modules */
typedef struct kerchunk_core kerchunk_core_t;

struct kerchunk_core {
    /* Event bus */
    int  (*subscribe)(kerchevt_type_t type, kerchevt_handler_t handler, void *ud);
    int  (*unsubscribe)(kerchevt_type_t type, kerchevt_handler_t handler);
    void (*fire_event)(const kerchevt_t *evt);

    /* Outbound queue */
    int  (*queue_audio_file)(const char *path, int priority);
    int  (*queue_audio_buffer)(const int16_t *buf, size_t n, int priority, int flags);
    int  (*queue_tone)(int freq_hz, int duration_ms, int16_t amplitude, int priority);
    int  (*queue_silence)(int duration_ms, int priority);
    int  (*queue_flush)(void);
    int  (*queue_depth)(void);

    /* PTT control */
    void (*request_ptt)(const char *module_name);
    void (*release_ptt)(const char *module_name);
    int  (*is_receiving)(void);
    int  (*is_transmitting)(void);

    /* Audio tap (capture path — incoming RX audio) */
    int  (*audio_tap_register)(kerchevt_handler_t handler, void *ud);
    void (*audio_tap_unregister)(kerchevt_handler_t handler);

    /* Playback tap (outbound TX audio — queue drain) */
    int  (*playback_tap_register)(kerchevt_handler_t handler, void *ud);
    void (*playback_tap_unregister)(kerchevt_handler_t handler);

    /* Config access */
    const char *(*config_get)(const char *section, const char *key);
    int  (*config_get_int)(const char *section, const char *key, int def);
    int  (*config_get_duration_ms)(const char *section, const char *key, int def);

    /* Logging */
    void (*log)(int level, const char *module, const char *fmt, ...);

    /* Timer API */
    int  (*timer_create)(int ms, int repeat, void (*cb)(void *), void *ud);
    void (*timer_cancel)(int timer_id);

    /* TTS (set by mod_tts if loaded, NULL otherwise) */
    int  (*tts_speak)(const char *text, int priority);

    /* SSE broadcast for large/custom payloads (set by mod_web when loaded).
     *
     * Producers call this with an event-type string and a self-contained
     * JSON payload. mod_web wraps it as {"type":<type>,"data":<payload>,
     * "ts":<now_us>}, pushes it to every connected SSE client, and caches
     * the last value per event-type so future connectors get a replay on
     * their initial burst — no client polling needed.
     *
     *   admin_only = 0 → broadcast to public (/api/events) and admin
     *   admin_only = 1 → admin (/admin/api/events) only
     *
     * Safe to call from any thread. The payload is copied; the caller's
     * buffer does not need to outlive the call. */
    void (*sse_publish)(const char *event_type,
                        const char *payload_json,
                        int admin_only);

    /* DTMF command registration (provided by mod_dtmfcmd when loaded) */
    int  (*dtmf_register)(const char *default_pattern, int event_offset,
                          const char *description, const char *config_key);
    int  (*dtmf_unregister)(const char *pattern);

    /* User database */
    const kerchunk_user_t *(*user_lookup_by_id)(int user_id);
    const kerchunk_user_t *(*user_lookup_by_ani)(const char *ani);
    int  (*user_count)(void);

    /* Runtime sample rate (set once at startup) */
    int sample_rate;
    int frame_samples;

    /* Wall-clock scheduler */
    int  (*schedule_at)(const struct timespec *when,
                        void (*cb)(void *), void *ud);
    int  (*schedule_aligned)(int align_ms, int offset_ms, int repeat,
                             void (*cb)(void *), void *ud);
    void (*schedule_cancel)(int sched_id);

    /* Managed threads */
    int  (*thread_create)(const char *name,
                          void *(*fn)(void *), void *ud);
    void (*thread_stop)(int tid);
    int  (*thread_should_stop)(int tid);
    void (*thread_join)(int tid);
    int  (*thread_count)(void);
};

/* Unified command dispatch — core commands + module commands */
int kerchunk_dispatch_command(int argc, const char **argv, kerchunk_resp_t *resp);

/* Event-to-JSON serializer (src/kerchunk_evt_json.c) */
int kerchevt_to_json(const kerchevt_t *evt, char *buf, size_t max);

/* Core scrambler hooks (audio thread access) */
typedef void (*kerchunk_scrambler_fn)(int16_t *buf, size_t n, void *ctx);
void  kerchunk_core_set_rx_scrambler(kerchunk_scrambler_fn fn, void *ctx);
kerchunk_scrambler_fn kerchunk_core_get_rx_scrambler(void **ctx);
void  kerchunk_core_set_tx_scrambler(kerchunk_scrambler_fn fn, void *ctx);
kerchunk_scrambler_fn kerchunk_core_get_tx_scrambler(void **ctx);

/* Core emergency flag */
void kerchunk_core_set_emergency(int active);
int  kerchunk_core_get_emergency(void);

/* Core OTP elevated session state */
void kerchunk_core_set_otp_elevated(int user_id, int elevated);
int  kerchunk_core_get_otp_elevated(int user_id);

/* Core sample rate setter */
void kerchunk_core_set_sample_rate(int rate);

/* Core config accessors */
kerchunk_config_t *kerchunk_core_get_config(void);
kerchunk_config_t *kerchunk_core_get_users_config(void);
void kerchunk_core_set_users_config(kerchunk_config_t *cfg);

/* Config access mutex */
void kerchunk_core_lock_config(void);
void kerchunk_core_unlock_config(void);

#endif /* KERCHUNK_H */
