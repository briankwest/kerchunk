/*
 * test_integ_timer.c — Integration tests for the timer subsystem
 *
 * Tests the real kerchunk_timer_* functions (not the mock).
 */

#include <unistd.h>
#include <string.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

/* Timer functions (internal, no public header) */
extern void kerchunk_timer_init(void);
extern void kerchunk_timer_shutdown(void);
extern int  kerchunk_timer_create(int ms, int repeat, void (*cb)(void *), void *ud);
extern void kerchunk_timer_cancel(int timer_id);
extern void kerchunk_timer_tick(void);

static int g_cb_count;

static void timer_test_cb(void *ud)
{
    (*(int *)ud)++;
}

void test_integ_timer(void)
{
    kerchunk_timer_init();

    /* 1. create returns a valid (>0) ID */
    test_begin("timer: create returns valid ID");
    g_cb_count = 0;
    int id = kerchunk_timer_create(1, 0, timer_test_cb, &g_cb_count);
    test_assert(id > 0, "invalid timer ID");
    test_end();

    /* 2. fires after interval elapses */
    test_begin("timer: fires after interval");
    usleep(2000);           /* 2 ms — timer was 1 ms */
    kerchunk_timer_tick();
    test_assert(g_cb_count == 1, "timer did not fire");
    test_end();

    /* 3. cancel prevents firing */
    test_begin("timer: cancel prevents firing");
    g_cb_count = 0;
    id = kerchunk_timer_create(1, 0, timer_test_cb, &g_cb_count);
    kerchunk_timer_cancel(id);
    usleep(2000);
    kerchunk_timer_tick();
    test_assert(g_cb_count == 0, "cancelled timer fired");
    test_end();

    /* 4. repeating timer fires multiple times */
    test_begin("timer: repeat fires multiple times");
    g_cb_count = 0;
    id = kerchunk_timer_create(1, 1, timer_test_cb, &g_cb_count);
    usleep(2000);
    kerchunk_timer_tick();
    test_assert(g_cb_count >= 1, "first fire failed");
    usleep(2000);
    kerchunk_timer_tick();
    test_assert(g_cb_count >= 2, "repeat fire failed");
    kerchunk_timer_cancel(id);
    test_end();

    kerchunk_timer_shutdown();
}
