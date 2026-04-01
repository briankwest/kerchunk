/*
 * kerchunk_sched.c — Wall-clock scheduler (CLOCK_REALTIME)
 *
 * Coexists with the monotonic timer system. Both fire callbacks
 * on the main thread via their respective tick functions.
 */

#include "kerchunk_sched.h"
#include "kerchunk_log.h"
#include <string.h>
#include <pthread.h>

#define LOG_MOD "sched"
#define MAX_SCHEDULES 64

typedef struct {
	int      active;
	int      id;
	int      repeat;
	int      align_ms;          /* 0 = absolute, >0 = aligned */
	int      offset_ms;
	struct timespec fire_time;
	void   (*callback)(void *);
	void    *userdata;
} sched_entry_t;

static sched_entry_t g_schedules[MAX_SCHEDULES];
static int g_next_id = 1;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static int ts_cmp(const struct timespec *a, const struct timespec *b)
{
	if (a->tv_sec != b->tv_sec)
		return (a->tv_sec < b->tv_sec) ? -1 : 1;
	if (a->tv_nsec != b->tv_nsec)
		return (a->tv_nsec < b->tv_nsec) ? -1 : 1;
	return 0;
}

static void compute_next_boundary(sched_entry_t *s)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	int64_t epoch_ms = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
	int64_t align = (int64_t)s->align_ms;
	int64_t offset = (int64_t)s->offset_ms;

	/* Next boundary strictly in the future */
	int64_t next = ((epoch_ms / align) + 1) * align + offset;

	/* If offset pushed us into the past, advance one more period */
	while (next <= epoch_ms)
		next += align;

	s->fire_time.tv_sec = (time_t)(next / 1000);
	s->fire_time.tv_nsec = (long)((next % 1000) * 1000000);
}

int kerchunk_sched_init(void)
{
	pthread_mutex_lock(&g_mutex);
	memset(g_schedules, 0, sizeof(g_schedules));
	g_next_id = 1;
	pthread_mutex_unlock(&g_mutex);
	return 0;
}

void kerchunk_sched_shutdown(void)
{
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_SCHEDULES; i++)
		g_schedules[i].active = 0;
	pthread_mutex_unlock(&g_mutex);
}

int kerchunk_sched_at(const struct timespec *when,
                      void (*cb)(void *), void *ud)
{
	if (!when || !cb) return -1;

	pthread_mutex_lock(&g_mutex);
	int slot = -1;
	for (int i = 0; i < MAX_SCHEDULES; i++) {
		if (!g_schedules[i].active) { slot = i; break; }
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_mutex);
		return -1;
	}

	sched_entry_t *s = &g_schedules[slot];
	s->active = 1;
	s->id = g_next_id++;
	s->repeat = 0;
	s->align_ms = 0;
	s->offset_ms = 0;
	s->fire_time = *when;
	s->callback = cb;
	s->userdata = ud;

	int id = s->id;
	pthread_mutex_unlock(&g_mutex);
	return id;
}

int kerchunk_sched_aligned(int align_ms, int offset_ms, int repeat,
                           void (*cb)(void *), void *ud)
{
	if (align_ms <= 0 || !cb) return -1;

	pthread_mutex_lock(&g_mutex);
	int slot = -1;
	for (int i = 0; i < MAX_SCHEDULES; i++) {
		if (!g_schedules[i].active) { slot = i; break; }
	}
	if (slot < 0) {
		pthread_mutex_unlock(&g_mutex);
		return -1;
	}

	sched_entry_t *s = &g_schedules[slot];
	s->active = 1;
	s->id = g_next_id++;
	s->repeat = repeat;
	s->align_ms = align_ms;
	s->offset_ms = offset_ms;
	s->callback = cb;
	s->userdata = ud;

	compute_next_boundary(s);

	int id = s->id;
	pthread_mutex_unlock(&g_mutex);
	return id;
}

void kerchunk_sched_cancel(int id)
{
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_SCHEDULES; i++) {
		if (g_schedules[i].active && g_schedules[i].id == id) {
			g_schedules[i].active = 0;
			break;
		}
	}
	pthread_mutex_unlock(&g_mutex);
}

void kerchunk_sched_tick(void)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);

	/* Scan outside the lock, fire callbacks outside too.
	 * Copy entries to fire to avoid holding lock during callback. */
	sched_entry_t fire[MAX_SCHEDULES];
	int nfire = 0;

	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_SCHEDULES; i++) {
		sched_entry_t *s = &g_schedules[i];
		if (!s->active) continue;
		if (ts_cmp(&now, &s->fire_time) >= 0) {
			fire[nfire++] = *s;

			if (s->repeat && s->align_ms > 0) {
				compute_next_boundary(s);
			} else {
				s->active = 0;
			}
		}
	}
	pthread_mutex_unlock(&g_mutex);

	for (int i = 0; i < nfire; i++)
		fire[i].callback(fire[i].userdata);
}

int kerchunk_sched_count(void)
{
	int n = 0;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_SCHEDULES; i++)
		if (g_schedules[i].active) n++;
	pthread_mutex_unlock(&g_mutex);
	return n;
}

void kerchunk_sched_iter(
    void (*cb)(int id, const char *type, const struct timespec *next,
               int repeat, void *ud),
    void *ud)
{
	if (!cb) return;
	pthread_mutex_lock(&g_mutex);
	for (int i = 0; i < MAX_SCHEDULES; i++) {
		sched_entry_t *s = &g_schedules[i];
		if (!s->active) continue;
		const char *type = s->align_ms > 0 ? "aligned" : "absolute";
		cb(s->id, type, &s->fire_time, s->repeat, ud);
	}
	pthread_mutex_unlock(&g_mutex);
}
