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

    for (int i = 0; i < count; i++)
        snapshot[i].handler(evt, snapshot[i].userdata);
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
