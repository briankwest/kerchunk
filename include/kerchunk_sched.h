/*
 * kerchunk_sched.h — Wall-clock scheduler and managed thread pool
 */

#ifndef KERCHUNK_SCHED_H
#define KERCHUNK_SCHED_H

#include <time.h>
#include <stdint.h>

/* ── Wall-clock scheduler ── */

int  kerchunk_sched_init(void);
void kerchunk_sched_shutdown(void);

/* Schedule callback at absolute UTC time (one-shot). Returns id >= 0. */
int  kerchunk_sched_at(const struct timespec *when,
                       void (*cb)(void *), void *ud);

/* Schedule callback aligned to wall-clock boundary.
 * align_ms:  period (e.g. 600000 for 10 min)
 * offset_ms: offset from boundary (can be negative for early fire)
 * repeat:    1 = re-arm to next boundary after fire
 * Returns id >= 0. */
int  kerchunk_sched_aligned(int align_ms, int offset_ms, int repeat,
                            void (*cb)(void *), void *ud);

void kerchunk_sched_cancel(int id);

/* Call from main loop every tick */
void kerchunk_sched_tick(void);

int  kerchunk_sched_count(void);
void kerchunk_sched_iter(
    void (*cb)(int id, const char *type, const struct timespec *next,
               int repeat, void *ud),
    void *ud);

/* ── Managed threads ── */

int  kerchunk_threads_init(void);
void kerchunk_threads_shutdown(void);

int  kerchunk_thread_create(const char *name,
                            void *(*fn)(void *), void *ud);
void kerchunk_thread_stop(int tid);
int  kerchunk_thread_should_stop(int tid);
void kerchunk_thread_join(int tid);
int  kerchunk_thread_count(void);
void kerchunk_thread_iter(
    void (*cb)(int id, const char *name, int running,
               uint64_t start_us, void *ud),
    void *ud);

#endif /* KERCHUNK_SCHED_H */
