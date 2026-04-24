/*
 * kerchunk_core.c — Core API implementation (passed to modules)
 *
 * Wires the kerchunk_core_t function pointers to actual implementations.
 */

#include "kerchunk.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <time.h>

#define LOG_MOD "core"

/* Config access mutex — protects config reads/writes across threads */
static pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;

void kerchunk_core_lock_config(void) { pthread_mutex_lock(&g_config_mutex); }
void kerchunk_core_unlock_config(void) { pthread_mutex_unlock(&g_config_mutex); }

/* External declarations for timer */
extern void kerchunk_timer_init(void);
extern void kerchunk_timer_shutdown(void);
extern int  kerchunk_timer_create(int ms, int repeat, void (*cb)(void *), void *ud);
extern void kerchunk_timer_cancel(int timer_id);

/* Global state */
static kerchunk_config_t *g_config;
static int g_ptt_active;
static int g_ptt_refcount;
static int g_cor_active;

/* Emergency flag (set by mod_emergency, read by other modules).
 * g_emergency_expires_at is the wall-clock time (epoch seconds) at
 * which the auto-deactivate timer is scheduled to fire — 0 when no
 * timer is armed. mod_emergency owns both writes; everyone else
 * reads via the getters. */
static int    g_emergency_active;
static time_t g_emergency_expires_at;

/* Audio tap (capture) — accessed from audio thread (dispatch) and
 * main thread (register/unregister). Protected by mutex + snapshot. */
#define MAX_TAPS 8
static kerchevt_handler_t g_taps[MAX_TAPS];
static void            *g_tap_ud[MAX_TAPS];
static int              g_tap_count;

/* Playback tap (queue drain) */
static kerchevt_handler_t g_play_taps[MAX_TAPS];
static void            *g_play_tap_ud[MAX_TAPS];
static int              g_play_tap_count;

static pthread_mutex_t  g_tap_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t  g_ptt_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Core API function implementations ── */

static int core_subscribe(kerchevt_type_t type, kerchevt_handler_t handler, void *ud)
{
    return kerchevt_subscribe(type, handler, ud);
}

static int core_unsubscribe(kerchevt_type_t type, kerchevt_handler_t handler)
{
    return kerchevt_unsubscribe(type, handler);
}

static void core_fire_event(const kerchevt_t *evt)
{
    kerchevt_fire(evt);
}

static int core_queue_audio_file(const char *path, int priority)
{
    return kerchunk_queue_add_file(path, priority);
}

static int core_queue_audio_buffer(const int16_t *buf, size_t n, int priority, int flags)
{
    return kerchunk_queue_add_buffer(buf, n, priority, flags);
}

static int core_queue_tone(int freq_hz, int duration_ms, int16_t amplitude, int priority)
{
    return kerchunk_queue_add_tone(freq_hz, duration_ms, amplitude, priority);
}

static int core_queue_silence(int duration_ms, int priority)
{
    return kerchunk_queue_add_silence(duration_ms, priority);
}

static int core_queue_flush(void)
{
    return kerchunk_queue_flush();
}

static int core_queue_depth(void)
{
    return kerchunk_queue_depth();
}

static void core_request_ptt(const char *module_name)
{
    pthread_mutex_lock(&g_ptt_mutex);
    g_ptt_refcount++;
    if (!g_ptt_active) {
        g_ptt_active = 1;
        kerchunk_hid_set_ptt(1);
        struct timespec _ts;
        clock_gettime(CLOCK_MONOTONIC, &_ts);
        kerchevt_t evt = {
            .type = KERCHEVT_PTT_ASSERT,
            .timestamp_us = (uint64_t)_ts.tv_sec * 1000000
                          + (uint64_t)_ts.tv_nsec / 1000,
        };
        kerchevt_fire(&evt);
    }
    KERCHUNK_LOG_D(LOG_MOD, "PTT asserted by %s (refs=%d)",
                 module_name ? module_name : "?", g_ptt_refcount);
    pthread_mutex_unlock(&g_ptt_mutex);
}

static void core_release_ptt(const char *module_name)
{
    pthread_mutex_lock(&g_ptt_mutex);
    if (g_ptt_refcount > 0)
        g_ptt_refcount--;
    KERCHUNK_LOG_D(LOG_MOD, "PTT released by %s (refs=%d)",
                 module_name ? module_name : "?", g_ptt_refcount);
    if (g_ptt_refcount == 0 && g_ptt_active) {
        g_ptt_active = 0;
        kerchunk_hid_set_ptt(0);
        struct timespec _ts2;
        clock_gettime(CLOCK_MONOTONIC, &_ts2);
        kerchevt_t evt = {
            .type = KERCHEVT_PTT_DROP,
            .timestamp_us = (uint64_t)_ts2.tv_sec * 1000000
                          + (uint64_t)_ts2.tv_nsec / 1000,
        };
        kerchevt_fire(&evt);
    }
    pthread_mutex_unlock(&g_ptt_mutex);
}

static int core_is_receiving(void)
{
    return g_cor_active;
}

static int core_is_transmitting(void)
{
    return g_ptt_active;
}

static int core_audio_tap_register(kerchevt_handler_t handler, void *ud)
{
    pthread_mutex_lock(&g_tap_mutex);
    if (g_tap_count >= MAX_TAPS) {
        pthread_mutex_unlock(&g_tap_mutex);
        return -1;
    }
    g_taps[g_tap_count]   = handler;
    g_tap_ud[g_tap_count] = ud;
    g_tap_count++;
    pthread_mutex_unlock(&g_tap_mutex);
    return 0;
}

static void core_audio_tap_unregister(kerchevt_handler_t handler)
{
    pthread_mutex_lock(&g_tap_mutex);
    for (int i = 0; i < g_tap_count; i++) {
        if (g_taps[i] == handler) {
            for (int j = i; j < g_tap_count - 1; j++) {
                g_taps[j]   = g_taps[j + 1];
                g_tap_ud[j] = g_tap_ud[j + 1];
            }
            g_tap_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_tap_mutex);
}

static int core_playback_tap_register(kerchevt_handler_t handler, void *ud)
{
    pthread_mutex_lock(&g_tap_mutex);
    if (g_play_tap_count >= MAX_TAPS) {
        pthread_mutex_unlock(&g_tap_mutex);
        return -1;
    }
    g_play_taps[g_play_tap_count]   = handler;
    g_play_tap_ud[g_play_tap_count] = ud;
    g_play_tap_count++;
    pthread_mutex_unlock(&g_tap_mutex);
    return 0;
}

static void core_playback_tap_unregister(kerchevt_handler_t handler)
{
    pthread_mutex_lock(&g_tap_mutex);
    for (int i = 0; i < g_play_tap_count; i++) {
        if (g_play_taps[i] == handler) {
            for (int j = i; j < g_play_tap_count - 1; j++) {
                g_play_taps[j]   = g_play_taps[j + 1];
                g_play_tap_ud[j] = g_play_tap_ud[j + 1];
            }
            g_play_tap_count--;
            break;
        }
    }
    pthread_mutex_unlock(&g_tap_mutex);
}

static const char *core_config_get(const char *section, const char *key)
{
    return kerchunk_config_get(g_config, section, key);
}

static int core_config_get_int(const char *section, const char *key, int def)
{
    return kerchunk_config_get_int(g_config, section, key, def);
}

static int core_config_get_duration_ms(const char *section, const char *key, int def)
{
    return kerchunk_config_get_duration_ms(g_config, section, key, def);
}

static void core_log(int level, const char *module, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kerchunk_log_msg(level, module, "%s", buf);
}

static int core_timer_create_fn(int ms, int repeat, void (*cb)(void *), void *ud)
{
    return kerchunk_timer_create(ms, repeat, cb, ud);
}

static void core_timer_cancel_fn(int timer_id)
{
    kerchunk_timer_cancel(timer_id);
}

static const kerchunk_user_t *core_user_lookup_by_id(int user_id)
{
    return kerchunk_user_lookup_by_id(user_id);
}

static const kerchunk_user_t *core_user_lookup_by_ani(const char *ani)
{
    return kerchunk_user_lookup_by_ani(ani);
}

static int core_user_count(void)
{
    return kerchunk_user_count();
}

/* ── Global core instance ── */

static kerchunk_core_t g_core = {
    .subscribe          = core_subscribe,
    .unsubscribe        = core_unsubscribe,
    .fire_event         = core_fire_event,
    .queue_audio_file   = core_queue_audio_file,
    .queue_audio_buffer = core_queue_audio_buffer,
    .queue_tone         = core_queue_tone,
    .queue_silence      = core_queue_silence,
    .queue_flush        = core_queue_flush,
    .queue_depth        = core_queue_depth,
    .request_ptt        = core_request_ptt,
    .release_ptt        = core_release_ptt,
    .is_receiving       = core_is_receiving,
    .is_transmitting    = core_is_transmitting,
    .audio_tap_register      = core_audio_tap_register,
    .audio_tap_unregister    = core_audio_tap_unregister,
    .playback_tap_register   = core_playback_tap_register,
    .playback_tap_unregister = core_playback_tap_unregister,
    .config_get             = core_config_get,
    .config_get_int         = core_config_get_int,
    .config_get_duration_ms = core_config_get_duration_ms,
    .log                    = core_log,
    .tts_speak          = NULL,  /* Set by mod_tts if loaded */
    .sse_publish        = NULL,  /* Set by mod_web if loaded */
    .dtmf_register      = NULL,
    .dtmf_unregister    = NULL,
    .timer_create       = core_timer_create_fn,
    .timer_cancel       = core_timer_cancel_fn,
    .user_lookup_by_id    = core_user_lookup_by_id,
    .user_lookup_by_ani   = core_user_lookup_by_ani,
    .user_count           = core_user_count,
    .sample_rate          = 48000,
    .frame_samples        = 960,
    .schedule_at          = kerchunk_sched_at,
    .schedule_aligned     = kerchunk_sched_aligned,
    .schedule_cancel      = kerchunk_sched_cancel,
    .thread_create        = kerchunk_thread_create,
    .thread_stop          = kerchunk_thread_stop,
    .thread_should_stop   = kerchunk_thread_should_stop,
    .thread_join          = kerchunk_thread_join,
    .thread_count         = kerchunk_thread_count,
};

/* Public accessors */
kerchunk_core_t *kerchunk_core_get(void)
{
    return &g_core;
}

void kerchunk_core_set_sample_rate(int rate)
{
    g_core.sample_rate   = rate;
    g_core.frame_samples = (rate * 20) / 1000;
}

void kerchunk_core_set_config(kerchunk_config_t *cfg)
{
    g_config = cfg;
}

void kerchunk_core_set_cor(int active)
{
    g_cor_active = active;
}

int kerchunk_core_get_ptt(void)
{
    return g_ptt_active;
}

/* Dispatch audio to tap handlers (called from audio thread).
 * Snapshot under lock, dispatch outside lock to avoid holding
 * mutex during potentially slow callbacks. */
void kerchunk_core_dispatch_taps(const kerchevt_t *evt)
{
    kerchevt_handler_t snap[MAX_TAPS];
    void *snap_ud[MAX_TAPS];
    int n;

    pthread_mutex_lock(&g_tap_mutex);
    n = g_tap_count;
    for (int i = 0; i < n; i++) {
        snap[i]    = g_taps[i];
        snap_ud[i] = g_tap_ud[i];
    }
    pthread_mutex_unlock(&g_tap_mutex);

    for (int i = 0; i < n; i++)
        snap[i](evt, snap_ud[i]);
}

void kerchunk_core_dispatch_playback_taps(const kerchevt_t *evt)
{
    kerchevt_handler_t snap[MAX_TAPS];
    void *snap_ud[MAX_TAPS];
    int n;

    pthread_mutex_lock(&g_tap_mutex);
    n = g_play_tap_count;
    for (int i = 0; i < n; i++) {
        snap[i]    = g_play_taps[i];
        snap_ud[i] = g_play_tap_ud[i];
    }
    pthread_mutex_unlock(&g_tap_mutex);

    for (int i = 0; i < n; i++)
        snap[i](evt, snap_ud[i]);
}

/* Scrambler hooks */
static kerchunk_scrambler_fn g_rx_scrambler_fn;
static void                *g_rx_scrambler_ctx;
static kerchunk_scrambler_fn g_tx_scrambler_fn;
static void                *g_tx_scrambler_ctx;

void kerchunk_core_set_rx_scrambler(kerchunk_scrambler_fn fn, void *ctx)
{
    g_rx_scrambler_fn  = fn;
    g_rx_scrambler_ctx = ctx;
}

kerchunk_scrambler_fn kerchunk_core_get_rx_scrambler(void **ctx)
{
    if (ctx) *ctx = g_rx_scrambler_ctx;
    return g_rx_scrambler_fn;
}

void kerchunk_core_set_tx_scrambler(kerchunk_scrambler_fn fn, void *ctx)
{
    g_tx_scrambler_fn  = fn;
    g_tx_scrambler_ctx = ctx;
}

kerchunk_scrambler_fn kerchunk_core_get_tx_scrambler(void **ctx)
{
    if (ctx) *ctx = g_tx_scrambler_ctx;
    return g_tx_scrambler_fn;
}

/* Emergency flag */
void kerchunk_core_set_emergency(int active)
{
    g_emergency_active = active;
    if (!active) g_emergency_expires_at = 0;
}

int kerchunk_core_get_emergency(void)
{
    return g_emergency_active;
}

void kerchunk_core_set_emergency_expires_at(time_t when)
{
    g_emergency_expires_at = when;
}

time_t kerchunk_core_get_emergency_expires_at(void)
{
    return g_emergency_expires_at;
}

/* OTP elevated session state — decoupled from user count.
 * Small fixed array of {user_id, elevated} pairs since
 * simultaneous OTP sessions are rare (usually 0-2). */
#define MAX_OTP_ELEVATED 64
static struct { int user_id; int elevated; } g_otp_sessions[MAX_OTP_ELEVATED];

void kerchunk_core_set_otp_elevated(int user_id, int elevated)
{
    if (user_id <= 0) return;
    for (int i = 0; i < MAX_OTP_ELEVATED; i++) {
        if (g_otp_sessions[i].user_id == user_id) {
            g_otp_sessions[i].elevated = elevated;
            return;
        }
    }
    if (!elevated) return;  /* no slot needed for clearing */
    for (int i = 0; i < MAX_OTP_ELEVATED; i++) {
        if (g_otp_sessions[i].user_id == 0) {
            g_otp_sessions[i].user_id = user_id;
            g_otp_sessions[i].elevated = elevated;
            return;
        }
    }
}

int kerchunk_core_get_otp_elevated(int user_id)
{
    if (user_id <= 0) return 0;
    for (int i = 0; i < MAX_OTP_ELEVATED; i++) {
        if (g_otp_sessions[i].user_id == user_id)
            return g_otp_sessions[i].elevated;
    }
    return 0;
}

kerchunk_config_t *kerchunk_core_get_config(void)
{
    return g_config;
}

static kerchunk_config_t *g_users_config;

kerchunk_config_t *kerchunk_core_get_users_config(void)
{
    return g_users_config ? g_users_config : g_config;
}

void kerchunk_core_set_users_config(kerchunk_config_t *cfg)
{
    g_users_config = cfg;
}
