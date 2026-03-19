/*
 * test_integ_cwid.c — Integration tests for mod_cwid
 *
 * Tests: timer fires CW ID when idle, defers when busy,
 *        pending clears after send, immediate send on IDLE transition.
 */

#include "test_integ_mock.h"

/* Pull in the module source */
#include "../modules/mod_cwid.c"

void test_integ_cwid_module(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "repeater", "cwid_interval", "600000");
    kerchunk_config_set(cfg, "repeater", "cwid_wpm", "20");
    kerchunk_config_set(cfg, "repeater", "cwid_freq", "800");
    kerchunk_config_set(cfg, "general", "callsign", "TEST");

    mod_cwid.load(&g_mock_core);
    cwid_configure(cfg);

    /* 1. Timer created on configure */
    test_begin("cwid: timer created on configure");
    test_assert(g_timer_id >= 0, "timer not created");
    test_end();

    /* 2. Timer fires CW ID when idle (not receiving, not transmitting) */
    test_begin("cwid: timer sends CW ID when idle");
    g_mock.receiving = 0;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    g_mock.silence_calls = 0;
    mock_fire_timer(g_timer_id);
    test_assert(g_mock.buffer_calls >= 1, "no CW audio queued");
    test_assert(g_mock.silence_calls >= 1, "no lead-in silence");
    test_assert(g_pending == 0, "pending flag not cleared");
    test_end();

    /* 3. Timer defers when receiving */
    test_begin("cwid: timer defers when receiving");
    g_mock.receiving = 1;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    mock_fire_timer(g_timer_id);
    test_assert(g_pending == 1, "pending not set");
    test_assert(g_mock.buffer_calls == 0, "CW ID sent while receiving");
    test_end();

    /* 4. Pending CW ID sent on tail start */
    test_begin("cwid: pending sent on tail start");
    g_mock.receiving = 0;
    g_mock.buffer_calls = 0;
    mock_fire_simple(KERCHEVT_TAIL_START);
    test_assert(g_mock.buffer_calls >= 1, "CW ID not sent on tail");
    test_assert(g_pending == 0, "pending not cleared after tail");
    test_end();

    /* 5. Timer defers when transmitting */
    test_begin("cwid: timer defers when transmitting");
    g_mock.receiving = 0;
    g_mock.transmitting = 1;
    g_mock.buffer_calls = 0;
    mock_fire_timer(g_timer_id);
    test_assert(g_pending == 1, "pending not set");
    test_assert(g_mock.buffer_calls == 0, "CW ID sent while transmitting");
    test_end();

    /* 6. Pending CW ID sent on state change to IDLE */
    test_begin("cwid: pending sent on IDLE transition");
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    g_mock.ptt_requested = 0;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 1, .new_state = 0 }, /* 0 = IDLE */
        };
        kerchevt_fire(&e);
    }
    test_assert(g_mock.buffer_calls >= 1, "CW ID not sent on IDLE");
    test_assert(g_mock.ptt_requested >= 1, "PTT not requested");
    test_assert(g_pending == 0, "pending not cleared");
    test_end();

    /* 7. No CW ID on IDLE if not pending */
    test_begin("cwid: no send on IDLE if not pending");
    g_mock.buffer_calls = 0;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 1, .new_state = 0 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_mock.buffer_calls == 0, "unexpected CW ID send");
    test_end();

    /* 8. CW ID interval capped at 15 min (FCC 95.1751) */
    test_begin("cwid: interval capped at 15 min");
    kerchunk_config_set(cfg, "repeater", "cwid_interval", "1800000");
    cwid_configure(cfg);
    test_assert(g_cwid_interval_ms == 900000, "interval not capped");
    test_end();

    mod_cwid.unload();
    kerchevt_shutdown();
    kerchunk_config_destroy(cfg);
}
