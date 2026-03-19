/*
 * test_integ_emergency.c — Integration tests for mod_emergency
 *
 * Tests: activate, auto-deactivate timeout, deactivate via DTMF.
 */

#include "test_integ_mock.h"

/* Pull in the module source */
#include "../modules/mod_emergency.c"

void test_integ_emergency(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "emergency", "timeout", "1800000");
    kerchunk_config_set(cfg, "general", "sounds_dir", "./sounds");

    mod_emergency.load(&g_mock_core);
    emergency_configure(cfg);

    /* 1. Emergency activation */
    test_begin("emergency: *911# activates");
    {
        kerchevt_t e = { .type = DTMF_EVT_EMERGENCY_ON };
        kerchevt_fire(&e);
    }
    test_assert(kerchunk_core_get_emergency() == 1, "not active");
    test_assert(g_timer >= 0, "timeout timer not started");
    test_assert(g_mock.file_calls >= 1, "no activation sound");
    test_end();

    /* 2. Auto-deactivate on timeout */
    test_begin("emergency: auto-deactivates on timeout");
    mock_fire_timer(g_timer);
    test_assert(kerchunk_core_get_emergency() == 0, "still active");
    test_assert(g_timer == -1, "timer not cleared");
    test_end();

    /* 3. Manual deactivation */
    test_begin("emergency: *910# deactivates");
    g_mock.file_calls = 0;
    /* Re-activate first */
    {
        kerchevt_t e = { .type = DTMF_EVT_EMERGENCY_ON };
        kerchevt_fire(&e);
    }
    test_assert(kerchunk_core_get_emergency() == 1, "not re-activated");
    g_mock.file_calls = 0;
    {
        kerchevt_t e = { .type = DTMF_EVT_EMERGENCY_OFF };
        kerchevt_fire(&e);
    }
    test_assert(kerchunk_core_get_emergency() == 0, "not deactivated");
    test_assert(g_mock.file_calls >= 1, "no deactivation sound");
    test_end();

    mod_emergency.unload();
    kerchevt_shutdown();
    kerchunk_config_destroy(cfg);
}
