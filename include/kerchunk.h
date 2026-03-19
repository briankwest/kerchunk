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

/* Version */
#define KERCHUNK_VERSION_MAJOR 1
#define KERCHUNK_VERSION_MINOR 0
#define KERCHUNK_VERSION_PATCH 0
#define KERCHUNK_VERSION_STRING "1.0.0"

/* Frame size: 20ms at 8 kHz = 160 samples */
#define KERCHUNK_FRAME_SAMPLES 160
#define KERCHUNK_FRAME_MS      20
#define KERCHUNK_SAMPLE_RATE   8000

/* Core API passed to modules */
typedef struct kerchunk_core kerchunk_core_t;

struct kerchunk_core {
    /* Event bus */
    int  (*subscribe)(kerchevt_type_t type, kerchevt_handler_t handler, void *ud);
    int  (*unsubscribe)(kerchevt_type_t type, kerchevt_handler_t handler);
    void (*fire_event)(const kerchevt_t *evt);

    /* Outbound queue */
    int  (*queue_audio_file)(const char *path, int priority);
    int  (*queue_audio_buffer)(const int16_t *buf, size_t n, int priority);
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

    /* Logging */
    void (*log)(int level, const char *module, const char *fmt, ...);

    /* Timer API */
    int  (*timer_create)(int ms, int repeat, void (*cb)(void *), void *ud);
    void (*timer_cancel)(int timer_id);

    /* TTS (set by mod_tts if loaded, NULL otherwise) */
    int  (*tts_speak)(const char *text, int priority);

    /* User database */
    const kerchunk_user_t *(*user_lookup_by_id)(int user_id);
    const kerchunk_user_t *(*user_lookup_by_ani)(const char *ani);
    int  (*user_count)(void);
};

/* Unified command dispatch — core commands + module commands */
int kerchunk_dispatch_command(int argc, const char **argv, kerchunk_resp_t *resp);

/* Event-to-JSON serializer (src/kerchunk_evt_json.c) */
int kerchevt_to_json(const kerchevt_t *evt, char *buf, size_t max);

/* TX encoder types */
#define KERCHUNK_TX_ENC_NONE  0
#define KERCHUNK_TX_ENC_CTCSS 1
#define KERCHUNK_TX_ENC_DCS   2

/* Core TX encoder state (audio thread access) */
void  kerchunk_core_set_tx_encoder(void *enc, int type);
void *kerchunk_core_get_tx_encoder(int *type);

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

/* Core config accessors */
kerchunk_config_t *kerchunk_core_get_config(void);
kerchunk_config_t *kerchunk_core_get_users_config(void);
void kerchunk_core_set_users_config(kerchunk_config_t *cfg);

/* Config access mutex */
void kerchunk_core_lock_config(void);
void kerchunk_core_unlock_config(void);

#endif /* KERCHUNK_H */
