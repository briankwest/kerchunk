/*
 * test_repeater.c — Repeater state machine tests
 */

#include "kerchunk_events.h"
#include <stdio.h>
#include <string.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

static int g_last_old_state = -1;
static int g_last_new_state = -1;
static int g_state_changes  = 0;
static int g_tail_started   = 0;
static int g_timeout_fired  = 0;

static void state_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_last_old_state = evt->state.old_state;
    g_last_new_state = evt->state.new_state;
    g_state_changes++;
}

static void tail_handler(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_tail_started++;
}

static void timeout_handler(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_timeout_fired++;
}

enum { RPT_IDLE, RPT_RECEIVING, RPT_TAIL_WAIT, RPT_HANG_WAIT, RPT_RX_TIMEOUT };

void test_repeater(void)
{
    kerchevt_init();
    kerchevt_subscribe(KERCHEVT_RX_STATE_CHANGE, state_handler, NULL);
    kerchevt_subscribe(KERCHEVT_TAIL_START, tail_handler, NULL);
    kerchevt_subscribe(KERCHEVT_RX_TIMEOUT, timeout_handler, NULL);

    test_begin("state change subscribers registered");
    test_assert(kerchevt_subscriber_count(KERCHEVT_RX_STATE_CHANGE) == 1, "sub count wrong");
    test_end();

    test_begin("COR_ASSERT event fires without crash");
    kerchevt_t cor_assert = { .type = KERCHEVT_COR_ASSERT, .cor = { .active = 1 } };
    kerchevt_fire(&cor_assert);
    test_end();

    test_begin("COR_DROP event fires without crash");
    kerchevt_t cor_drop = { .type = KERCHEVT_COR_DROP, .cor = { .active = 0 } };
    kerchevt_fire(&cor_drop);
    test_end();

    test_begin("state change event carries correct data");
    g_state_changes = 0;
    kerchevt_t sc = {
        .type = KERCHEVT_RX_STATE_CHANGE,
        .state = { .old_state = RPT_IDLE, .new_state = RPT_RECEIVING },
    };
    kerchevt_fire(&sc);
    test_assert(g_state_changes == 1, "not received");
    test_assert(g_last_old_state == RPT_IDLE, "old state wrong");
    test_assert(g_last_new_state == RPT_RECEIVING, "new state wrong");
    test_end();

    test_begin("TAIL_START event propagates");
    g_tail_started = 0;
    kerchevt_t tail = { .type = KERCHEVT_TAIL_START };
    kerchevt_fire(&tail);
    test_assert(g_tail_started == 1, "tail not received");
    test_end();

    test_begin("TIMEOUT event propagates");
    g_timeout_fired = 0;
    kerchevt_t timeout = { .type = KERCHEVT_RX_TIMEOUT };
    kerchevt_fire(&timeout);
    test_assert(g_timeout_fired == 1, "timeout not received");
    test_end();

    test_begin("RECEIVING -> TAIL_WAIT transition");
    sc.state.old_state = RPT_RECEIVING;
    sc.state.new_state = RPT_TAIL_WAIT;
    kerchevt_fire(&sc);
    test_assert(g_last_new_state == RPT_TAIL_WAIT, "wrong state");
    test_end();

    test_begin("TAIL_WAIT -> IDLE transition");
    sc.state.old_state = RPT_TAIL_WAIT;
    sc.state.new_state = RPT_IDLE;
    kerchevt_fire(&sc);
    test_assert(g_last_new_state == RPT_IDLE, "wrong state");
    test_end();

    kerchevt_shutdown();
}
