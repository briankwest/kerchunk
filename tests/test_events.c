/*
 * test_events.c — Event bus tests
 */

#include "kerchunk_events.h"
#include <stdio.h>
#include <string.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

static int g_handler_called;
static kerchevt_type_t g_last_type;
static char g_last_digit;

static void test_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_handler_called++;
    g_last_type = evt->type;
}

static void digit_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_handler_called++;
    g_last_digit = evt->dtmf.digit;
}

void test_events(void)
{
    test_begin("init and shutdown");
    test_assert(kerchevt_init() == 0, "init failed");
    kerchevt_shutdown();
    test_end();

    test_begin("subscribe and fire");
    kerchevt_init();
    g_handler_called = 0;
    kerchevt_subscribe(KERCHEVT_COR_ASSERT, test_handler, NULL);
    kerchevt_t evt = { .type = KERCHEVT_COR_ASSERT };
    kerchevt_fire(&evt);
    test_assert(g_handler_called == 1, "handler not called");
    test_end();

    test_begin("handler receives correct event type");
    test_assert(g_last_type == KERCHEVT_COR_ASSERT, "wrong type");
    test_end();

    test_begin("multiple subscribers same event");
    g_handler_called = 0;
    kerchevt_subscribe(KERCHEVT_COR_ASSERT, digit_handler, NULL);
    kerchevt_fire(&evt);
    test_assert(g_handler_called == 2, "both handlers not called");
    test_end();

    test_begin("unsubscribe");
    g_handler_called = 0;
    kerchevt_unsubscribe(KERCHEVT_COR_ASSERT, test_handler);
    kerchevt_fire(&evt);
    test_assert(g_handler_called == 1, "unsubscribe failed");
    test_end();

    test_begin("subscriber count");
    test_assert(kerchevt_subscriber_count(KERCHEVT_COR_ASSERT) == 1, "wrong count");
    test_end();

    test_begin("event type isolation");
    g_handler_called = 0;
    kerchevt_t other = { .type = KERCHEVT_COR_DROP };
    kerchevt_fire(&other);
    test_assert(g_handler_called == 0, "wrong event fired");
    test_end();

    test_begin("DTMF event carries digit data");
    kerchevt_subscribe(KERCHEVT_DTMF_DIGIT, digit_handler, NULL);
    kerchevt_t dtmf_evt = { .type = KERCHEVT_DTMF_DIGIT, .dtmf = { .digit = '5' } };
    g_handler_called = 0;
    g_last_digit = '\0';
    kerchevt_fire(&dtmf_evt);
    test_assert(g_handler_called == 1 && g_last_digit == '5', "digit data wrong");
    test_end();

    test_begin("custom event type");
    g_handler_called = 0;
    kerchevt_subscribe(KERCHEVT_CUSTOM, test_handler, NULL);
    kerchevt_t custom = { .type = KERCHEVT_CUSTOM };
    kerchevt_fire(&custom);
    test_assert(g_handler_called == 1, "custom event failed");
    test_end();

    test_begin("fire with no subscribers (no crash)");
    kerchevt_t lonely = { .type = KERCHEVT_TIMEOUT };
    kerchevt_fire(&lonely);
    test_end();

    /* Auto-timestamp: events with timestamp_us=0 get stamped by fire() */
    test_begin("auto-timestamp on fire");
    kerchevt_t unstamped = { .type = KERCHEVT_COR_ASSERT, .timestamp_us = 0 };
    kerchevt_fire(&unstamped);
    test_assert(unstamped.timestamp_us > 0, "timestamp not set");
    test_end();

    /* Explicit timestamps preserved */
    test_begin("explicit timestamp preserved");
    kerchevt_t stamped = { .type = KERCHEVT_COR_DROP, .timestamp_us = 12345678 };
    kerchevt_fire(&stamped);
    test_assert(stamped.timestamp_us == 12345678, "timestamp overwritten");
    test_end();

    kerchevt_shutdown();
}
