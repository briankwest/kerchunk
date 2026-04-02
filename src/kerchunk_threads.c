/*
 * kerchunk_threads.c — Managed module thread pool
 *
 * Provides core visibility into module background threads:
 * named threads, graceful shutdown, health monitoring.
 */

#include "kerchunk_sched.h"
#include "kerchunk_log.h"
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#define LOG_MOD "threads"
#define MAX_THREADS 32

typedef struct {
	int             active;
	int             id;
	char            name[32];
	pthread_t       pthread;
	atomic_int      stop_requested;
	atomic_int      running;
	void          *(*fn)(void *);
	void           *userdata;
	uint64_t        start_time_us;
} managed_thread_t;

static managed_thread_t g_threads[MAX_THREADS];
static int g_next_id = 1;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void *thread_wrapper(void *arg)
{
	managed_thread_t *mt = (managed_thread_t *)arg;

#ifdef __linux__
	pthread_setname_np(pthread_self(), mt->name);
#elif defined(__APPLE__)
	pthread_setname_np(mt->name);
#endif

	KERCHUNK_LOG_I(LOG_MOD, "thread '%s' [%d] started", mt->name, mt->id);

	atomic_store(&mt->running, 1);
	mt->fn(mt->userdata);
	atomic_store(&mt->running, 0);

	KERCHUNK_LOG_I(LOG_MOD, "thread '%s' [%d] exited", mt->name, mt->id);
	return NULL;
}

int kerchunk_threads_init(void)
{
	pthread_mutex_lock(&g_mutex);
	memset(g_threads, 0, sizeof(g_threads));
	g_next_id = 1;
	pthread_mutex_unlock(&g_mutex);
	return 0;
}

void kerchunk_threads_shutdown(void)
{
	KERCHUNK_LOG_I(LOG_MOD, "shutting down managed threads");

	/* Signal all to stop */
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_THREADS; i++)
		if (g_threads[i].active)
			atomic_store(&g_threads[i].stop_requested, 1);
	pthread_mutex_unlock(&g_mutex);

	/* Join all with timeout */
	for (int i = 0; i < MAX_THREADS; i++) {
		if (!g_threads[i].active) continue;

#ifdef __linux__
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += 5;
		int rc = pthread_timedjoin_np(g_threads[i].pthread, NULL, &deadline);
		if (rc != 0) {
			KERCHUNK_LOG_W(LOG_MOD,
			               "thread '%s' [%d] did not exit in 5s",
			               g_threads[i].name, g_threads[i].id);
		}
#else
		pthread_join(g_threads[i].pthread, NULL);
#endif
		g_threads[i].active = 0;
	}
}

int kerchunk_thread_create(const char *name,
                           void *(*fn)(void *), void *ud)
{
	if (!name || !fn) return -1;

	pthread_mutex_lock(&g_mutex);
	int slot = -1;
	for (int i = 0; i < MAX_THREADS; i++) {
		if (!g_threads[i].active) { slot = i; break; }
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_mutex);
		KERCHUNK_LOG_E(LOG_MOD, "max threads reached (%d)", MAX_THREADS);
		return -1;
	}

	managed_thread_t *mt = &g_threads[slot];
	memset(mt, 0, sizeof(*mt));
	mt->active = 1;
	mt->id = g_next_id++;
	snprintf(mt->name, sizeof(mt->name), "%s", name);
	mt->fn = fn;
	mt->userdata = ud;
	atomic_store(&mt->stop_requested, 0);
	mt->start_time_us = now_us();

	int rc = pthread_create(&mt->pthread, NULL, thread_wrapper, mt);
	if (rc != 0) {
		mt->active = 0;
		pthread_mutex_unlock(&g_mutex);
		KERCHUNK_LOG_E(LOG_MOD, "pthread_create failed for '%s': %d", name, rc);
		return -1;
	}

	int id = mt->id;
	pthread_mutex_unlock(&g_mutex);
	return id;
}

void kerchunk_thread_stop(int tid)
{
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_THREADS; i++) {
		if (g_threads[i].active && g_threads[i].id == tid) {
			atomic_store(&g_threads[i].stop_requested, 1);
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
}

int kerchunk_thread_should_stop(int tid)
{
	/* No lock — stop_requested is atomic and read-only from worker */
	for (int i = 0; i < MAX_THREADS; i++) {
		if (g_threads[i].active && g_threads[i].id == tid)
			return atomic_load(&g_threads[i].stop_requested);
	}
	return 1; /* unknown tid = stop */
}

void kerchunk_thread_join(int tid)
{
	pthread_t pt = 0;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_THREADS; i++) {
		if (g_threads[i].active && g_threads[i].id == tid) {
			pt = g_threads[i].pthread;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);

	if (pt)
		pthread_join(pt, NULL);
}

int kerchunk_thread_count(void)
{
	int n = 0;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_THREADS; i++)
		if (g_threads[i].active && atomic_load(&g_threads[i].running)) n++;
	pthread_mutex_unlock(&g_mutex);
	return n;
}

void kerchunk_thread_iter(
    void (*cb)(int id, const char *name, int running,
               uint64_t start_us, void *ud),
    void *ud)
{
	if (!cb) return;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_THREADS; i++) {
		if (!g_threads[i].active) continue;
		cb(g_threads[i].id, g_threads[i].name,
		   atomic_load(&g_threads[i].running), g_threads[i].start_time_us, ud);
	}
	pthread_mutex_unlock(&g_mutex);
}
