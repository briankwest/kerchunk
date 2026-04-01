# Kerchunk: Scheduled Callbacks & Managed Module Threads

## Problem

Two gaps in the kerchunk core API:

1. **No wall-clock scheduling.** The existing `timer_create(ms, repeat, cb, ud)` uses CLOCK_MONOTONIC — great for intervals, but can't target a specific UTC time. FLEX frame alignment needs "fire at 21:15:03.750 UTC". CW ID clock-alignment is a hack (compute offset, one-shot, then repeat). Weather/time announcements at specific minutes past the hour are similarly awkward.

2. **No managed threads.** Modules that need background work (mod_nws curl fetches, mod_webhook HTTP posts, mod_sdr SDR I/O) spawn raw pthreads with no core visibility. The core can't gracefully shut them down, monitor health, or log their state. FreeSWITCH solves this with a managed thread pool where modules register threads that the core tracks and shuts down cleanly.

## Proposal 1: Scheduled Callbacks

### API Additions to `kerchunk_core_t`

```c
/* Schedule a callback at a specific wall-clock time (UTC).
 *
 * when:     absolute time (CLOCK_REALTIME)
 * cb:       callback function
 * ud:       user data
 * Returns:  schedule ID (>= 0) or -1 on error
 *
 * The callback fires on the main thread, same as timer callbacks.
 * One-shot only — re-schedule from within the callback if needed.
 * Accuracy depends on main loop tick (20ms) + clock sync (NTP).
 */
int  (*schedule_at)(const struct timespec *when,
                    void (*cb)(void *), void *ud);

/* Schedule a callback aligned to a wall-clock boundary.
 *
 * align_ms:  alignment period in milliseconds
 *            (e.g., 1875 for FLEX frames, 600000 for 10-min CW ID)
 * offset_ms: offset from boundary (e.g., -200 for 200ms early)
 * repeat:    1 = re-arm after each fire, 0 = one-shot
 * cb/ud:     callback and user data
 * Returns:   schedule ID (>= 0) or -1 on error
 *
 * The first fire is at the next boundary. For repeat, it re-arms
 * to the following boundary (not now + align_ms, avoiding drift).
 *
 * Example: FLEX frame 103 fires every 240 seconds (1 cycle)
 * at second offset (103 * 1.875) = 193.125s within each cycle:
 *   schedule_aligned(240000, 193125 - 200, 1, flex_tx_cb, ctx);
 */
int  (*schedule_aligned)(int align_ms, int offset_ms, int repeat,
                         void (*cb)(void *), void *ud);

/* Cancel a scheduled callback */
void (*schedule_cancel)(int sched_id);
```

### Implementation (`src/kerchunk_timer.c`)

Extend the existing timer array with a second pool for wall-clock schedules:

```c
typedef struct {
    int      active;
    int      id;
    int      repeat;
    int      align_ms;          /* 0 = absolute, >0 = aligned */
    int      offset_ms;
    struct timespec fire_time;   /* CLOCK_REALTIME */
    void   (*callback)(void *);
    void    *userdata;
} sched_entry_t;

#define MAX_SCHEDULES 64
static sched_entry_t g_schedules[MAX_SCHEDULES];
```

`kerchunk_timer_tick()` already runs every 20ms on the main thread. Add a second scan for wall-clock schedules:

```c
void kerchunk_timer_tick(void)
{
    /* Existing monotonic timers... */

    /* Wall-clock schedules */
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    for (int i = 0; i < MAX_SCHEDULES; i++) {
        sched_entry_t *s = &g_schedules[i];
        if (!s->active) continue;
        if (timespec_cmp(&now, &s->fire_time) >= 0) {
            s->active = 0;  /* deactivate before callback */
            s->callback(s->userdata);

            /* re-arm aligned schedules */
            if (s->repeat && s->align_ms > 0) {
                advance_to_next_boundary(s);
                s->active = 1;
            }
        }
    }
}
```

### FLEX Usage

```c
/* In mod_flex configure(): */
static void flex_frame_cb(void *ud)
{
    flex_sched_entry_t *entry = (flex_sched_entry_t *)ud;
    /* Audio is pre-encoded. Just queue it. */
    g_core->queue_audio_buffer(entry->pcm, entry->ns, KERCHUNK_PRI_NORMAL);
    free(entry);
}

/* On flex send command: */
static int flex_tx_scheduled(uint32_t capcode, ...)
{
    /* Pre-encode and modulate */
    flex_sched_entry_t *entry = malloc(sizeof(*entry) + pcm_size);
    encode_and_modulate(entry, capcode, ...);

    /* Compute fire time: next occurrence of this capcode's frame */
    struct timespec when;
    flex_next_frame_time(capcode, &now, &when);

    /* Subtract 200ms for PTT + preamble lead time */
    when.tv_nsec -= 200000000;
    if (when.tv_nsec < 0) { when.tv_sec--; when.tv_nsec += 1000000000; }

    g_core->schedule_at(&when, flex_frame_cb, entry);
}
```

### CW ID Usage (Simplifies Existing Code)

```c
/* Current: calculate offset, one-shot, then repeat — fragile */
/* New: */
g_core->schedule_aligned(g_cwid_interval_ms, 0, 1, cwid_timer_cb, NULL);
/* Fires at every 10-minute boundary, automatically re-arms to next boundary */
```

---

## Proposal 2: Managed Module Threads

### Problem

Current pattern in mod_nws, mod_webhook, mod_sdr:

```c
static pthread_t g_thread;
static volatile int g_running = 1;

static void *worker(void *arg) {
    while (g_running) { /* ... */ }
    return NULL;
}

static int mod_load(...) {
    pthread_create(&g_thread, NULL, worker, NULL);
}

static void mod_unload(void) {
    g_running = 0;
    pthread_join(&g_thread, NULL);
}
```

Problems:
- Core has no visibility into module threads
- No graceful shutdown signal — modules use `volatile int` ad-hoc
- No health monitoring (hung thread detection)
- Thread names not set (hard to debug with `top` or `ps`)
- No standardized logging of thread lifecycle

### API Additions to `kerchunk_core_t`

```c
/* Thread entry point. Return value is ignored.
 * Check core->thread_should_stop(tid) periodically. */
typedef void *(*kerchunk_thread_fn)(void *ud);

/* Create a managed thread.
 *
 * name:  human-readable name (shown in logs, ps)
 * fn:    thread entry point
 * ud:    user data passed to fn
 * Returns: thread ID (>= 0) or -1 on error
 *
 * The core tracks the thread and joins it on shutdown.
 * The thread should check thread_should_stop() periodically
 * and exit cleanly when it returns true.
 */
int  (*thread_create)(const char *name, kerchunk_thread_fn fn, void *ud);

/* Signal a managed thread to stop (non-blocking).
 * The thread should notice on its next check of thread_should_stop(). */
void (*thread_stop)(int tid);

/* Check if the thread should exit. Call from within the thread. */
int  (*thread_should_stop)(int tid);

/* Wait for a managed thread to finish (blocking). */
void (*thread_join)(int tid);

/* Number of active managed threads. */
int  (*thread_count)(void);
```

### Implementation (`src/kerchunk_threads.c`)

```c
#include <pthread.h>

typedef struct {
    int             active;
    int             id;
    char            name[32];
    pthread_t       pthread;
    volatile int    stop_requested;
    volatile int    running;       /* 1 = thread is alive */
    kerchunk_thread_fn fn;
    void           *userdata;
    uint64_t        start_time_us;
} managed_thread_t;

#define MAX_THREADS 32
static managed_thread_t g_threads[MAX_THREADS];
static pthread_mutex_t  g_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Wrapper that sets thread name and tracks lifecycle */
static void *thread_wrapper(void *arg)
{
    managed_thread_t *mt = (managed_thread_t *)arg;

    /* Set thread name for ps/top/gdb */
    pthread_setname_np(pthread_self(), mt->name);

    kerchunk_log(KERCHUNK_LOG_INFO, "core",
                 "thread '%s' [%d] started", mt->name, mt->id);

    mt->running = 1;
    mt->fn(mt->userdata);
    mt->running = 0;

    kerchunk_log(KERCHUNK_LOG_INFO, "core",
                 "thread '%s' [%d] exited", mt->name, mt->id);
    return NULL;
}

int kerchunk_thread_create(const char *name, kerchunk_thread_fn fn, void *ud)
{
    pthread_mutex_lock(&g_thread_mutex);
    int slot = find_free_slot();
    if (slot < 0) { pthread_mutex_unlock(&g_thread_mutex); return -1; }

    managed_thread_t *mt = &g_threads[slot];
    mt->active = 1;
    mt->id = slot;
    snprintf(mt->name, sizeof(mt->name), "%s", name);
    mt->fn = fn;
    mt->userdata = ud;
    mt->stop_requested = 0;
    mt->start_time_us = now_us();

    pthread_create(&mt->pthread, NULL, thread_wrapper, mt);
    pthread_mutex_unlock(&g_thread_mutex);
    return slot;
}

/* Called during core shutdown — stop and join all threads */
void kerchunk_threads_shutdown(void)
{
    /* Signal all threads to stop */
    for (int i = 0; i < MAX_THREADS; i++)
        if (g_threads[i].active)
            g_threads[i].stop_requested = 1;

    /* Join all with timeout */
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!g_threads[i].active) continue;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 5;  /* 5 second timeout */

        int rc = pthread_timedjoin_np(g_threads[i].pthread,
                                      NULL, &deadline);
        if (rc != 0)
            kerchunk_log(KERCHUNK_LOG_WARNING, "core",
                         "thread '%s' did not exit in 5s, abandoning",
                         g_threads[i].name);
        g_threads[i].active = 0;
    }
}
```

### Module Usage

```c
/* mod_nws.c — before: raw pthread */
/* After: managed thread */

static int g_tid = -1;

static void *nws_worker(void *ud)
{
    while (!g_core->thread_should_stop(g_tid)) {
        poll_nws_alerts();
        /* Sleep with periodic stop check */
        for (int i = 0; i < 300 && !g_core->thread_should_stop(g_tid); i++)
            usleep(100000);  /* 100ms × 300 = 30s poll interval */
    }
    return NULL;
}

static int nws_load(kerchunk_core_t *core)
{
    g_core = core;
    g_tid = core->thread_create("nws-poll", nws_worker, NULL);
    return 0;
}

static void nws_unload(void)
{
    if (g_tid >= 0) {
        g_core->thread_stop(g_tid);
        g_core->thread_join(g_tid);
        g_tid = -1;
    }
}
```

### FLEX SDR Receiver Thread

A `mod_flex_rx` module could use managed threads for continuous SDR reception:

```c
static int g_sdr_tid = -1;

static void *sdr_rx_worker(void *ud)
{
    rtlsdr_dev_t *dev = open_sdr(...);

    while (!g_core->thread_should_stop(g_sdr_tid)) {
        read_iq_and_demod(dev);
        feed_decoder();
    }

    rtlsdr_close(dev);
    return NULL;
}

static int mod_load(kerchunk_core_t *core)
{
    g_sdr_tid = core->thread_create("flex-sdr", sdr_rx_worker, NULL);
    return 0;
}
```

---

## CLI Additions

### `threads` Command

```
kerchunk> threads
ID  Name          State    Uptime
 0  nws-poll      running  2h 15m
 1  webhook-send  running  2h 15m
 2  flex-sdr      running  0h 03m

3 managed threads active
```

### `schedule` Command

```
kerchunk> schedule
ID  Type      Next Fire (UTC)          Repeat  Module
 0  aligned   2026-04-01 21:15:00.000  yes     mod_cwid (600s boundary)
 1  absolute  2026-04-01 21:15:03.550  no      mod_flex (frame 103)

2 scheduled callbacks active
```

---

## Integration with Existing Timer System

The wall-clock scheduler coexists with the monotonic timer:

| Feature | `timer_create` (existing) | `schedule_at` / `schedule_aligned` (new) |
|---------|---------------------------|------------------------------------------|
| Clock | CLOCK_MONOTONIC | CLOCK_REALTIME |
| Use case | Intervals, delays | Wall-clock alignment, UTC targets |
| Drift | None (monotonic) | Follows NTP adjustments |
| Resolution | 20ms | 20ms |
| NTP required | No | Yes (for accuracy) |
| Survives suspend | Yes | Re-syncs on wake |

Both fire callbacks on the main thread. Both are scanned in `kerchunk_timer_tick()`.

---

## Shutdown Sequence

```
1. Fire KERCHEVT_SHUTDOWN event
2. Cancel all timers (monotonic + wall-clock)
3. Signal all managed threads to stop
4. Wait up to 5s for each thread to exit
5. Unload modules (module->unload())
6. Close audio, HID, socket
7. Exit
```

---

## Files to Create/Modify

### New Files
| File | Purpose |
|------|---------|
| `src/kerchunk_sched.c` | Wall-clock scheduler implementation |
| `src/kerchunk_threads.c` | Managed thread pool implementation |

### Modified Files
| File | Change |
|------|--------|
| `include/kerchunk.h` | Add `schedule_at`, `schedule_aligned`, `schedule_cancel`, `thread_create`, `thread_stop`, `thread_should_stop`, `thread_join`, `thread_count` to `kerchunk_core_t` |
| `src/main.c` | Call `kerchunk_sched_tick()` in main loop, call `kerchunk_threads_shutdown()` on exit |
| `src/kerchunk_timer.c` | Factor out common tick logic, add sched scan |
| `src/Makefile.am` | Add new source files |

### Modules to Migrate (Optional, Non-Breaking)
| Module | Migration |
|--------|-----------|
| `mod_cwid` | Replace clock-alignment hack with `schedule_aligned()` |
| `mod_nws` | Replace raw pthread with `thread_create()` |
| `mod_webhook` | Replace raw pthread with `thread_create()` |
| `mod_sdr` | Replace raw pthread with `thread_create()` |
| `mod_flex` | Use `schedule_at()` for frame-aligned TX |
| `mod_time` | Optionally use `schedule_aligned()` for on-the-hour announcements |

### Backward Compatibility
- `timer_create` / `timer_cancel` unchanged — existing modules keep working
- New APIs are additive — no breaking changes
- Modules migrate at their own pace
