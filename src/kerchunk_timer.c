/*
 * kerchunk_timer.c — Simple timer wheel for module timers
 *
 * Timers are checked each tick (20ms). A linear scan is fine
 * for the expected count (<100 timers).
 */

#include "kerchunk_log.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

#define LOG_MOD "timer"
#define MAX_TIMERS 128

typedef struct {
    int      active;
    int      id;
    int      interval_ms;
    int      repeat;
    uint64_t next_fire_us;
    void   (*callback)(void *);
    void    *userdata;
} timer_entry_t;

static timer_entry_t g_timers[MAX_TIMERS];
static int g_next_id = 1;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

void kerchunk_timer_init(void)
{
    memset(g_timers, 0, sizeof(g_timers));
    g_next_id = 1;
}

void kerchunk_timer_shutdown(void)
{
    memset(g_timers, 0, sizeof(g_timers));
}

int kerchunk_timer_create(int ms, int repeat, void (*cb)(void *), void *ud)
{
    if (!cb || ms <= 0)
        return -1;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].active) {
            g_timers[i].active       = 1;
            g_timers[i].id           = g_next_id++;
            g_timers[i].interval_ms  = ms;
            g_timers[i].repeat       = repeat;
            g_timers[i].next_fire_us = now_us() + (uint64_t)ms * 1000;
            g_timers[i].callback     = cb;
            g_timers[i].userdata     = ud;
            return g_timers[i].id;
        }
    }

    KERCHUNK_LOG_E(LOG_MOD, "no free timer slots");
    return -1;
}

void kerchunk_timer_cancel(int timer_id)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (g_timers[i].active && g_timers[i].id == timer_id) {
            g_timers[i].active = 0;
            return;
        }
    }
}

void kerchunk_timer_tick(void)
{
    uint64_t t = now_us();

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!g_timers[i].active)
            continue;
        if (t >= g_timers[i].next_fire_us) {
            /* Snapshot callback before modifying state —
             * the callback may cancel this or other timers */
            void (*cb)(void *) = g_timers[i].callback;
            void *ud = g_timers[i].userdata;

            if (g_timers[i].repeat) {
                g_timers[i].next_fire_us = t + (uint64_t)g_timers[i].interval_ms * 1000;
            } else {
                g_timers[i].active = 0;  /* Deactivate before callback */
            }

            cb(ud);
        }
    }
}
