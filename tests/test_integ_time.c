/*
 * test_integ_time.c — Integration tests for mod_time
 *
 * Includes mod_time.c directly.
 * Tests announce output, busy-skip logic, and DTMF trigger.
 */

#include "test_integ_mock.h"

/* Pull in the module source */
#include "../modules/mod_time.c"

/* ---- entry point ---- */

void test_integ_time(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    mod_time.load(&g_mock_core);

    /* 1. time_announce queues audio files */
    test_begin("time: announce queues WAV files");
    g_mock.file_calls = 0;
    g_enabled = 1;
    snprintf(g_sounds_dir, sizeof(g_sounds_dir), "./sounds");
    time_announce();
    /* Minimum: tm_the_time_is + hour + minute-or-oclock + am/pm = 4+ files */
    test_assert(g_mock.file_calls >= 4, "too few WAV files queued");
    test_end();

    /* 2. timer skips when receiving */
    test_begin("time: timer skips when receiving");
    g_mock.file_calls = 0;
    g_mock.receiving = 1;
    time_timer_cb(NULL);
    test_assert(g_mock.file_calls == 0, "queued audio while receiving");
    g_mock.receiving = 0;
    test_end();

    /* 3. timer skips when transmitting */
    test_begin("time: timer skips when transmitting");
    g_mock.file_calls = 0;
    g_mock.transmitting = 1;
    time_timer_cb(NULL);
    test_assert(g_mock.file_calls == 0, "queued audio while transmitting");
    g_mock.transmitting = 0;
    test_end();

    /* 4. timer announces when idle */
    test_begin("time: timer announces when idle");
    g_mock.file_calls = 0;
    time_timer_cb(NULL);
    test_assert(g_mock.file_calls >= 4, "no audio when idle");
    test_end();

    /* 5. DTMF handler always announces (even if busy) */
    test_begin("time: DTMF announces regardless");
    g_mock.file_calls = 0;
    g_mock.transmitting = 1;
    kerchevt_t evt = { .type = (kerchevt_type_t)DTMF_EVT_TIME };
    kerchevt_fire(&evt);
    test_assert(g_mock.file_calls >= 4, "DTMF did not announce");
    g_mock.transmitting = 0;
    test_end();

    /* 6. timezone WAV queued when configured */
    test_begin("time: timezone WAV queued");
    g_mock.file_calls = 0;
    snprintf(g_timezone, sizeof(g_timezone), "central");
    time_announce();
    /* Last file should contain "tm_central" */
    test_assert(strstr(g_mock.last_file, "tm_central") != NULL,
                "timezone WAV not queued");
    test_end();

    /* 7. no timezone WAV when unconfigured */
    test_begin("time: no timezone when empty");
    g_timezone[0] = '\0';
    g_mock.file_calls = 0;
    time_announce();
    test_assert(strstr(g_mock.last_file, "tm_") != NULL, "no files queued");
    /* last file should be tm_am or tm_pm, not tm_<timezone> */
    test_assert(strstr(g_mock.last_file, "tm_am") != NULL ||
                strstr(g_mock.last_file, "tm_pm") != NULL,
                "unexpected last file");
    test_end();

    mod_time.unload();
    kerchevt_shutdown();
}
