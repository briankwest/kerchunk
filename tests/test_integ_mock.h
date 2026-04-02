/*
 * test_integ_mock.h — Mock core vtable for integration tests
 *
 * Provides a mock kerchunk_core_t that routes event bus and user DB
 * to real implementations but records all PTT, queue, timer, and
 * log calls for assertions.
 *
 * Include this header BEFORE including a module .c file.
 * KERCHUNK_MODULE_DEFINE is suppressed so module symbols stay static.
 */

#ifndef TEST_INTEG_MOCK_H
#define TEST_INTEG_MOCK_H

#include "kerchunk.h"
#include <stdio.h>

/* Suppress module export — included .c files stay static.
 * Reference the module def to avoid -Wunused-variable warnings. */
#undef KERCHUNK_MODULE_DEFINE
#define KERCHUNK_MODULE_DEFINE(mod) \
    __attribute__((unused)) static const void *_test_ref_##mod = &(mod)

/* Test harness (from test_main.c) */
extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);
extern void test_pass(void);
extern void test_fail(const char *msg);

#include <string.h>
#include <stdarg.h>

/* Suppress unused-function warnings — not every test uses every helper */
#define MOCK_UNUSED __attribute__((unused))

/* ------------------------------------------------------------------ */
/*  Mock timer                                                         */
/* ------------------------------------------------------------------ */

#define MOCK_MAX_TIMERS 32

typedef struct {
    int      active;
    int      id;
    int      ms;
    int      repeat;
    void   (*callback)(void *);
    void    *userdata;
} mock_timer_t;

/* ------------------------------------------------------------------ */
/*  Mock state                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    /* PTT */
    int ptt_requested;
    int ptt_released;
    int ptt_active;

    /* Queue */
    int     tone_calls;
    int     file_calls;
    int     buffer_calls;
    int     silence_calls;
    int     flush_calls;
    int     last_tone_freq;
    int     last_tone_dur;
    int16_t last_tone_amp;
    int     last_tone_pri;
    char    last_file[256];

    /* Audio tap */
    kerchevt_handler_t tap_handler;
    void *tap_ud;
    int   tap_registered;

    /* Timers */
    mock_timer_t timers[MOCK_MAX_TIMERS];
    int timer_next_id;

    /* Log */
    int  log_count;
    int  last_log_level;
    char last_log_msg[256];

    /* HW state */
    int receiving;
    int transmitting;

    /* Events the module fired through mock_fire_event */
    int           events_fired;
    kerchevt_type_t last_event_type;
} mock_state_t;

static mock_state_t  g_mock;
static kerchunk_core_t g_mock_core;

/* ------------------------------------------------------------------ */
/*  Mock function implementations                                      */
/* ------------------------------------------------------------------ */

static int mock_subscribe(kerchevt_type_t t, kerchevt_handler_t h, void *ud)
{
    return kerchevt_subscribe(t, h, ud);
}

static int mock_unsubscribe(kerchevt_type_t t, kerchevt_handler_t h)
{
    return kerchevt_unsubscribe(t, h);
}

static void mock_fire_event(const kerchevt_t *e)
{
    g_mock.events_fired++;
    g_mock.last_event_type = e->type;
    kerchevt_fire(e);
}

static int mock_queue_audio_file(const char *p, int pri)
{
    (void)pri;
    g_mock.file_calls++;
    if (p)
        snprintf(g_mock.last_file, sizeof(g_mock.last_file), "%s", p);
    return 0;
}

static int mock_queue_audio_buffer(const int16_t *b, size_t n, int pri, int flags)
{
    (void)b; (void)n; (void)pri; (void)flags;
    g_mock.buffer_calls++;
    return 0;
}

static int mock_queue_tone(int f, int d, int16_t a, int pri)
{
    g_mock.tone_calls++;
    g_mock.last_tone_freq = f;
    g_mock.last_tone_dur  = d;
    g_mock.last_tone_amp  = a;
    g_mock.last_tone_pri  = pri;
    return 0;
}

static int mock_queue_silence(int d, int pri)
{
    (void)d; (void)pri;
    g_mock.silence_calls++;
    return 0;
}

static int mock_queue_flush(void)  { g_mock.flush_calls++; return 0; }
static int mock_queue_depth(void)  { return 0; }

static void mock_request_ptt(const char *m)
{
    (void)m;
    g_mock.ptt_requested++;
    g_mock.ptt_active = 1;
}

static void mock_release_ptt(const char *m)
{
    (void)m;
    g_mock.ptt_released++;
    g_mock.ptt_active = 0;
}

static int mock_is_receiving(void)    { return g_mock.receiving; }
static int mock_is_transmitting(void) { return g_mock.transmitting; }

static int mock_audio_tap_register(kerchevt_handler_t h, void *ud)
{
    g_mock.tap_handler    = h;
    g_mock.tap_ud         = ud;
    g_mock.tap_registered = 1;
    return 0;
}

static void mock_audio_tap_unregister(kerchevt_handler_t h)
{
    (void)h;
    g_mock.tap_handler    = NULL;
    g_mock.tap_ud         = NULL;
    g_mock.tap_registered = 0;
}

static int mock_playback_tap_register(kerchevt_handler_t h, void *ud)
{
    (void)h; (void)ud;
    return 0;
}

static void mock_playback_tap_unregister(kerchevt_handler_t h)
{
    (void)h;
}

static const char *mock_config_get(const char *s, const char *k)
{
    (void)s; (void)k;
    return NULL;
}

static int mock_config_get_int(const char *s, const char *k, int d)
{
    (void)s; (void)k;
    return d;
}

static void mock_log(int lvl, const char *mod, const char *fmt, ...)
{
    (void)mod;
    g_mock.log_count++;
    g_mock.last_log_level = lvl;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_mock.last_log_msg, sizeof(g_mock.last_log_msg), fmt, ap);
    va_end(ap);
}

static int mock_dtmf_register(const char *p, int off, const char *d, const char *k) { (void)p;(void)off;(void)d;(void)k; return 0; }
static int mock_dtmf_unregister(const char *p) { (void)p; return 0; }

static int mock_timer_create(int ms, int repeat, void (*cb)(void *), void *ud)
{
    if (!cb || ms <= 0)
        return -1;
    for (int i = 0; i < MOCK_MAX_TIMERS; i++) {
        if (!g_mock.timers[i].active) {
            g_mock.timers[i] = (mock_timer_t){
                .active   = 1,
                .id       = ++g_mock.timer_next_id,
                .ms       = ms,
                .repeat   = repeat,
                .callback = cb,
                .userdata = ud,
            };
            return g_mock.timers[i].id;
        }
    }
    return -1;
}

static void mock_timer_cancel(int id)
{
    for (int i = 0; i < MOCK_MAX_TIMERS; i++) {
        if (g_mock.timers[i].active && g_mock.timers[i].id == id) {
            g_mock.timers[i].active = 0;
            return;
        }
    }
}

/* Mock schedule functions (use timer slots since behavior is identical) */
MOCK_UNUSED static int mock_schedule_at(const struct timespec *when,
                                        void (*cb)(void *), void *ud)
{
    (void)when;
    return mock_timer_create(0, 0, cb, ud);
}

MOCK_UNUSED static int mock_schedule_aligned(int align_ms, int offset_ms, int repeat,
                                             void (*cb)(void *), void *ud)
{
    (void)align_ms; (void)offset_ms;
    return mock_timer_create(0, repeat, cb, ud);
}

MOCK_UNUSED static void mock_schedule_cancel(int id) { mock_timer_cancel(id); }

/* Mock thread functions (no-ops in tests) */
MOCK_UNUSED static int mock_thread_create(const char *n, void *(*fn)(void *), void *ud)
{ (void)n; (void)fn; (void)ud; return 1; }
MOCK_UNUSED static void mock_thread_stop(int tid) { (void)tid; }
MOCK_UNUSED static int mock_thread_should_stop(int tid) { (void)tid; return 0; }
MOCK_UNUSED static void mock_thread_join(int tid) { (void)tid; }
MOCK_UNUSED static int mock_thread_count(void) { return 0; }

/* ------------------------------------------------------------------ */
/*  Test helpers                                                       */
/* ------------------------------------------------------------------ */

/* Fire a mock timer's callback (simulates time elapsing) */
MOCK_UNUSED static void mock_fire_timer(int id)
{
    for (int i = 0; i < MOCK_MAX_TIMERS; i++) {
        if (g_mock.timers[i].active && g_mock.timers[i].id == id) {
            void (*cb)(void *) = g_mock.timers[i].callback;
            void *ud           = g_mock.timers[i].userdata;
            if (!g_mock.timers[i].repeat)
                g_mock.timers[i].active = 0;
            cb(ud);
            return;
        }
    }
}

/* Find a timer by its callback function */
MOCK_UNUSED static int mock_find_timer(void (*cb)(void *))
{
    for (int i = 0; i < MOCK_MAX_TIMERS; i++) {
        if (g_mock.timers[i].active && g_mock.timers[i].callback == cb)
            return g_mock.timers[i].id;
    }
    return -1;
}

/* Check if a timer is still active */
MOCK_UNUSED static int mock_timer_active(int id)
{
    for (int i = 0; i < MOCK_MAX_TIMERS; i++) {
        if (g_mock.timers[i].active && g_mock.timers[i].id == id)
            return 1;
    }
    return 0;
}

/* Count active mock timers */
MOCK_UNUSED static int mock_active_timer_count(void)
{
    int n = 0;
    for (int i = 0; i < MOCK_MAX_TIMERS; i++)
        if (g_mock.timers[i].active) n++;
    return n;
}

/* Reset all mock state */
static void mock_reset(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
}

/* Initialize the mock core vtable */
static void mock_init_core(void)
{
    g_mock_core = (kerchunk_core_t){
        .subscribe           = mock_subscribe,
        .unsubscribe         = mock_unsubscribe,
        .fire_event          = mock_fire_event,
        .queue_audio_file    = mock_queue_audio_file,
        .queue_audio_buffer  = mock_queue_audio_buffer,
        .queue_tone          = mock_queue_tone,
        .queue_silence       = mock_queue_silence,
        .queue_flush         = mock_queue_flush,
        .queue_depth         = mock_queue_depth,
        .request_ptt         = mock_request_ptt,
        .release_ptt         = mock_release_ptt,
        .is_receiving        = mock_is_receiving,
        .is_transmitting     = mock_is_transmitting,
        .audio_tap_register      = mock_audio_tap_register,
        .audio_tap_unregister    = mock_audio_tap_unregister,
        .playback_tap_register   = mock_playback_tap_register,
        .playback_tap_unregister = mock_playback_tap_unregister,
        .config_get          = mock_config_get,
        .config_get_int      = mock_config_get_int,
        .log                 = mock_log,
        .tts_speak           = NULL,
        .dtmf_register       = mock_dtmf_register,
        .dtmf_unregister     = mock_dtmf_unregister,
        .timer_create        = mock_timer_create,
        .timer_cancel        = mock_timer_cancel,
        .user_lookup_by_id   = kerchunk_user_lookup_by_id,
        .user_lookup_by_ani  = kerchunk_user_lookup_by_ani,
        .user_count          = kerchunk_user_count,
        .sample_rate         = 48000,
        .frame_samples       = 960,
        .schedule_at         = mock_schedule_at,
        .schedule_aligned    = mock_schedule_aligned,
        .schedule_cancel     = mock_schedule_cancel,
        .thread_create       = mock_thread_create,
        .thread_stop         = mock_thread_stop,
        .thread_should_stop  = mock_thread_should_stop,
        .thread_join         = mock_thread_join,
        .thread_count        = mock_thread_count,
    };
}

/* Fire a bare event (simulates hardware/external stimulus) */
MOCK_UNUSED static void mock_fire_simple(kerchevt_type_t type)
{
    kerchevt_t e = { .type = type };
    kerchevt_fire(&e);
}

/* Fire a DTMF digit event */
MOCK_UNUSED static void mock_fire_dtmf(char digit)
{
    kerchevt_t e = {
        .type = KERCHEVT_DTMF_DIGIT,
        .dtmf = { .digit = digit, .duration_ms = 100 },
    };
    kerchevt_fire(&e);
}

#endif /* TEST_INTEG_MOCK_H */
