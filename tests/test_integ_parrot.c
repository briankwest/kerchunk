/*
 * test_integ_parrot.c — Integration tests for mod_parrot
 *
 * Tests: arming via DTMF, recording on COR, playback on drop,
 *        max duration cap, disarming without COR.
 */

#include "test_integ_mock.h"

/* Pull in the module source */
#include "../modules/mod_parrot.c"

void test_integ_parrot(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "parrot", "max_duration", "10");

    mod_parrot.load(&g_mock_core);
    parrot_configure(cfg);

    /* 1. *88# arms parrot */
    test_begin("parrot: *88# arms");
    {
        kerchevt_t e = { .type = DTMF_EVT_PARROT };
        kerchevt_fire(&e);
    }
    test_assert(g_armed == 1, "not armed");
    test_assert(g_mock.tone_calls >= 1, "no confirmation tone");
    test_end();

    /* 2. COR assert starts recording */
    test_begin("parrot: COR starts recording");
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_armed == 0, "still armed after COR");
    test_assert(g_recording == 1, "not recording");
    test_assert(g_mock.tap_registered == 1, "tap not registered");
    test_end();

    /* 3. Audio tap captures samples */
    test_begin("parrot: captures audio");
    {
        int16_t samples[160];
        for (int i = 0; i < 160; i++) samples[i] = (int16_t)(i * 10);
        kerchevt_t e = {
            .type = KERCHEVT_AUDIO_FRAME,
            .audio = { .samples = samples, .n = 160 },
        };
        /* Call the tap directly (simulating audio thread dispatch) */
        parrot_audio_tap(&e, NULL);
    }
    test_assert(g_len == 160, "wrong sample count");
    test_end();

    /* 4. COR drop triggers playback */
    test_begin("parrot: COR drop plays back");
    g_mock.buffer_calls = 0;
    g_mock.silence_calls = 0;
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_recording == 0, "still recording");
    test_assert(g_mock.tap_registered == 0, "tap still registered");
    test_assert(g_mock.buffer_calls == 1, "no buffer queued for playback");
    test_assert(g_mock.silence_calls >= 1, "no lead-in silence");
    test_end();

    /* 5. Not armed after playback (one-shot) */
    test_begin("parrot: not armed after use");
    test_assert(g_armed == 0, "still armed");
    test_assert(g_recording == 0, "still recording");
    test_end();

    /* 6. COR without arming does nothing */
    test_begin("parrot: COR without arm is ignored");
    g_mock.buffer_calls = 0;
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_recording == 0, "recording without arm");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_mock.buffer_calls == 0, "unexpected playback");
    test_end();

    /* 7. Max duration cap */
    test_begin("parrot: max duration caps buffer");
    {
        kerchevt_t e = { .type = DTMF_EVT_PARROT };
        kerchevt_fire(&e);
    }
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_recording == 1, "not recording");
    /* Feed more than max_duration worth of samples */
    for (int i = 0; i < 600; i++) {  /* 600 * 160 = 96000 > 80000 (10s) */
        int16_t samples[160] = {0};
        kerchevt_t e = {
            .type = KERCHEVT_AUDIO_FRAME,
            .audio = { .samples = samples, .n = 160 },
        };
        parrot_audio_tap(&e, NULL);
    }
    test_assert(g_len <= (size_t)(RATE * g_max_duration_s),
                "exceeded max duration");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_end();

    mod_parrot.unload();
    kerchevt_shutdown();
    kerchunk_config_destroy(cfg);
}
