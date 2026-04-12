/*
 * test_integ_voicemail.c — Integration tests for voicemail module
 *
 * Includes mod_voicemail.c directly.
 * Tests status tone, record start, COR-drop stop, caller tracking,
 * and the enabled flag.
 */

#include "test_integ_mock.h"

/* Pull in the module source */
#include "../modules/mod_voicemail.c"

/* ---- entry point ---- */

void test_integ_voicemail(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    /* Set up minimal user DB */
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "user.1", "name",      "Brian");
    kerchunk_config_set(cfg, "user.1", "access",    "2");
    kerchunk_config_set(cfg, "user.1", "voicemail", "1");
    kerchunk_config_set(cfg, "voicemail", "enabled",      "on");
    kerchunk_config_set(cfg, "voicemail", "max_messages", "20");
    kerchunk_config_set(cfg, "voicemail", "max_duration", "10");
    kerchunk_user_init(cfg);

    mod_voicemail.load(&g_mock_core);
    voicemail_configure(cfg);

    /* Set caller via event (mirrors mod_caller firing CALLER_IDENTIFIED) */
    {
        kerchevt_t e = { .type = KERCHEVT_CALLER_IDENTIFIED,
                        .caller = { .user_id = 1, .method = 1 } };
        kerchevt_fire(&e);
    }

    /* 1. status tone when no messages → low 400 Hz tone */
    test_begin("voicemail: status tone for zero messages");
    g_mock.tone_calls = 0;
    {
        kerchevt_t e = { .type = (kerchevt_type_t)DTMF_EVT_VOICEMAIL_STATUS };
        kerchevt_fire(&e);
    }
    test_assert(g_mock.tone_calls >= 1, "no tone played");
    test_assert(g_mock.last_tone_freq == 400, "wrong freq for no-messages");
    test_end();

    /* 2. record arms on DTMF, then begins on next COR_ASSERT.
     * The two-phase flow exists because mod_dtmfcmd's deferred dispatch
     * and mod_voicemail's COR_DROP handler both fire on the *same*
     * COR_DROP — without arming, recording would start and immediately
     * be saved (empty) on that single COR cycle. */
    test_begin("voicemail: record arms then starts on COR assert");
    g_mock.tone_calls = 0;
    {
        kerchevt_t e = { .type = (kerchevt_type_t)DTMF_EVT_VOICEMAIL_RECORD };
        kerchevt_fire(&e);
    }
    test_assert(g_record_armed == 1, "armed flag not set after dial");
    test_assert(g_recording == 0, "started recording too early");
    test_assert(g_mock.tap_registered == 0, "tap registered while only armed");
    test_assert(g_mock.tone_calls >= 1, "no prompt beep");

    /* User keys up — recording begins for real */
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_record_armed == 0, "armed flag not cleared");
    test_assert(g_recording == 1, "not recording after COR assert");
    test_assert(g_mock.tap_registered == 1, "audio tap not registered");
    test_end();

    /* 3. COR drop stops recording */
    test_begin("voicemail: COR drop stops recording");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_recording == 0, "still recording");
    test_assert(g_mock.tap_registered == 0, "tap still registered");
    test_end();

    /* 4. caller tracking via events.
     * mod_voicemail tracks the *last identified* caller, not the
     * "actively transmitting" one — CALLER_CLEARED is intentionally
     * ignored so login sessions persist across COR drops and the
     * deferred DTMF dispatch can still see the caller id when it
     * runs after CALLER_CLEARED on the same COR_DROP event. */
    test_begin("voicemail: caller tracking");
    {
        kerchevt_t e = { .type = KERCHEVT_CALLER_IDENTIFIED,
                        .caller = { .user_id = 5, .method = 1 } };
        kerchevt_fire(&e);
    }
    test_assert(g_current_caller_id == 5, "caller ID not set");
    {
        kerchevt_t e = { .type = KERCHEVT_CALLER_CLEARED };
        kerchevt_fire(&e);
    }
    test_assert(g_current_caller_id == 5, "caller ID cleared by CALLER_CLEARED (should persist)");
    /* A new identification updates it */
    {
        kerchevt_t e = { .type = KERCHEVT_CALLER_IDENTIFIED,
                        .caller = { .user_id = 7, .method = 1 } };
        kerchevt_fire(&e);
    }
    test_assert(g_current_caller_id == 7, "caller ID not updated by new identify");
    test_end();

    /* 5. disabled flag rejects commands with audible feedback.
     * The reject() helper plays a low-high tone pair (or speaks via
     * TTS when available — mock has tts_speak=NULL so we get tones).
     * The previous behavior of silently no-op'ing was a UX bug. */
    test_begin("voicemail: disabled audibly rejects commands");
    g_enabled = 0;
    g_current_caller_id = 1;
    g_mock.tone_calls = 0;
    {
        kerchevt_t e = { .type = (kerchevt_type_t)DTMF_EVT_VOICEMAIL_STATUS };
        kerchevt_fire(&e);
    }
    test_assert(g_mock.tone_calls >= 2, "no rejection tone pair played when disabled");
    test_end();

    mod_voicemail.unload();
    kerchevt_shutdown();
    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);
}
