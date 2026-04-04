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

    /* 1. Schedule created on configure */
    test_begin("cwid: timer created on configure");
    test_assert(g_sched_id >= 0, "schedule not created");
    test_end();

    /* 2. Schedule fires CW ID when idle (not receiving, not transmitting) */
    test_begin("cwid: timer sends CW ID when idle");
    g_mock.receiving = 0;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    g_mock.silence_calls = 0;
    cwid_timer_cb(NULL);
    test_assert(g_mock.buffer_calls >= 1, "no CW audio queued");
    test_assert(g_mock.silence_calls >= 1, "no lead-in silence");
    test_assert(g_pending == 0, "pending flag not cleared");
    test_end();

    /* 3. Schedule defers when receiving */
    test_begin("cwid: timer defers when receiving");
    g_mock.receiving = 1;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    cwid_timer_cb(NULL);
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

    /* 5. Schedule defers when transmitting */
    test_begin("cwid: timer defers when transmitting");
    g_mock.receiving = 0;
    g_mock.transmitting = 1;
    g_mock.buffer_calls = 0;
    cwid_timer_cb(NULL);
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
    /* PTT is managed by the queue drain engine, not by mod_cwid directly */
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

    /* --- Quiet hours tests --- */

    /* 9. Quiet hours disabled (-1): timer fires normally */
    test_begin("cwid: quiet hours disabled — timer fires");
    kerchunk_config_set(cfg, "repeater", "cwid_interval", "600000");
    kerchunk_config_set(cfg, "repeater", "quiet_start", "-1");
    kerchunk_config_set(cfg, "repeater", "quiet_end", "-1");
    mod_cwid.load(&g_mock_core);
    cwid_configure(cfg);
    g_mock.receiving = 0;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    cwid_timer_cb(NULL);
    test_assert(g_mock.buffer_calls >= 1, "CW ID should fire when quiet disabled");
    test_end();
    mod_cwid.unload();

    /* 10. Quiet hours covering all 24h: timer skips */
    test_begin("cwid: quiet hours all-day — timer skips");
    kerchunk_config_set(cfg, "repeater", "quiet_start", "0");
    kerchunk_config_set(cfg, "repeater", "quiet_end", "24");
    mod_cwid.load(&g_mock_core);
    cwid_configure(cfg);
    g_mock.receiving = 0;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    cwid_timer_cb(NULL);
    test_assert(g_mock.buffer_calls == 0, "CW ID should be skipped during quiet hours");
    test_end();

    /* 11. Pending CW ID suppressed during quiet hours (tail start) */
    test_begin("cwid: pending suppressed on tail during quiet hours");
    g_pending = 1;
    g_mock.buffer_calls = 0;
    mock_fire_simple(KERCHEVT_TAIL_START);
    test_assert(g_mock.buffer_calls == 0, "deferred CW ID should not fire during quiet");
    test_assert(g_pending == 1, "pending should remain set");
    test_end();

    /* 12. Pending CW ID suppressed during quiet hours (IDLE transition) */
    test_begin("cwid: pending suppressed on IDLE during quiet hours");
    g_mock.buffer_calls = 0;
    g_mock.ptt_requested = 0;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 1, .new_state = 0 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_mock.buffer_calls == 0, "deferred CW ID should not fire during quiet");
    test_assert(g_mock.ptt_requested == 0, "PTT should not be requested during quiet");
    test_end();
    mod_cwid.unload();

    /* 13. is_quiet_hour wraps midnight correctly */
    test_begin("cwid: quiet hour wrap-around logic");
    g_quiet_start = 22;
    g_quiet_end = 6;
    {
        time_t now = time(NULL);
        int hour = localtime(&now)->tm_hour;
        int expected = (hour >= 22 || hour < 6);
        test_assert(is_quiet_hour() == expected, "wrap-around mismatch for current hour");
    }
    test_end();

    /* 14. is_quiet_hour same-day range */
    test_begin("cwid: quiet hour same-day range");
    g_quiet_start = 9;
    g_quiet_end = 17;
    {
        time_t now = time(NULL);
        int hour = localtime(&now)->tm_hour;
        int expected = (hour >= 9 && hour < 17);
        test_assert(is_quiet_hour() == expected, "same-day range mismatch");
    }
    test_end();

    /* ── on_call mode tests ── */

    /* Reconfigure for on_call mode */
    kerchunk_config_set(cfg, "repeater", "cwid_mode", "on_call");
    kerchunk_config_set(cfg, "repeater", "cwid_interval", "600000");
    kerchunk_config_set(cfg, "repeater", "quiet_start", "-1");
    kerchunk_config_set(cfg, "repeater", "quiet_end", "-1");
    mod_cwid.load(&g_mock_core);
    cwid_configure(cfg);

    /* 15. on_call: starts in IDLE, no schedule timer */
    test_begin("cwid: on_call starts in IDLE");
    test_assert(g_mode == CWID_MODE_ON_CALL, "mode not on_call");
    test_assert(g_oc_state == OC_IDLE, "state not IDLE");
    test_assert(g_sched_id < 0, "schedule should not exist in on_call mode");
    test_end();

    /* 16. on_call: first key-up transitions IDLE→ACTIVE with pending */
    test_begin("cwid: on_call IDLE→ACTIVE on first keyup");
    g_mock.receiving = 1;
    g_mock.buffer_calls = 0;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 0, .new_state = 1 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_oc_state == OC_ACTIVE, "state not ACTIVE");
    test_assert(g_pending == 1, "pending not set on initial keyup");
    test_assert(g_oc_timer_id >= 0, "repeating timer not started");
    test_assert(g_mock.buffer_calls == 0, "should not ID while channel busy");
    test_end();

    /* 17. on_call: unkey sends pending ID, transitions to TAIL */
    test_begin("cwid: on_call ACTIVE→TAIL sends initial ID");
    g_mock.receiving = 0;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 1, .new_state = 0 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_oc_state == OC_TAIL, "state not TAIL");
    test_assert(g_mock.buffer_calls >= 1, "initial CW ID not sent");
    test_assert(g_pending == 0, "pending not cleared");
    test_end();

    /* 18. on_call: rekey from TAIL → ACTIVE without setting pending */
    test_begin("cwid: on_call TAIL→ACTIVE no pending on rekey");
    g_mock.receiving = 1;
    g_mock.buffer_calls = 0;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 0, .new_state = 1 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_oc_state == OC_ACTIVE, "state not ACTIVE");
    test_assert(g_pending == 0, "pending should NOT be set on rekey from TAIL");
    test_end();

    /* 19. on_call: unkey after rekey does NOT send CW ID */
    test_begin("cwid: on_call no ID on normal unkey");
    g_mock.receiving = 0;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 1, .new_state = 0 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_oc_state == OC_TAIL, "state not TAIL");
    test_assert(g_mock.buffer_calls == 0, "CW ID should NOT fire on every unkey");
    test_end();

    /* 20. on_call: repeating timer sets pending during ACTIVE */
    test_begin("cwid: on_call timer sets pending during conversation");
    /* Re-enter ACTIVE */
    g_mock.receiving = 1;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 0, .new_state = 1 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_oc_state == OC_ACTIVE, "state not ACTIVE");
    /* Simulate timer firing while channel is busy */
    g_mock.buffer_calls = 0;
    oc_active_tick(NULL);
    test_assert(g_pending == 1, "timer should set pending when busy");
    test_assert(g_mock.buffer_calls == 0, "should not send while receiving");
    test_end();

    /* 21. on_call: pending from timer sent on next unkey */
    test_begin("cwid: on_call timer-pending sent on unkey");
    g_mock.receiving = 0;
    g_mock.transmitting = 0;
    g_mock.buffer_calls = 0;
    {
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 1, .new_state = 0 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_mock.buffer_calls >= 1, "timer-pending CW ID not sent");
    test_assert(g_pending == 0, "pending not cleared");
    test_end();

    /* 22. on_call: tail timer fires → final ID → IDLE */
    test_begin("cwid: on_call tail timer final ID");
    g_mock.buffer_calls = 0;
    g_mock.receiving = 0;
    g_mock.transmitting = 0;
    oc_tail_expire(NULL);
    test_assert(g_oc_state == OC_IDLE, "state not IDLE after tail expire");
    test_assert(g_mock.buffer_calls >= 1, "final CW ID not sent");
    test_end();

    /* 23. on_call: stays silent in IDLE (no spurious IDs) */
    test_begin("cwid: on_call silent when idle");
    g_mock.buffer_calls = 0;
    {
        /* Fire a tick — nothing should happen */
        kerchevt_t e = {
            .type = KERCHEVT_STATE_CHANGE,
            .state = { .old_state = 0, .new_state = 0 },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_oc_state == OC_IDLE, "state should remain IDLE");
    test_assert(g_mock.buffer_calls == 0, "no CW ID in IDLE");
    test_end();

    mod_cwid.unload();
    kerchevt_shutdown();
    kerchunk_config_destroy(cfg);
}
