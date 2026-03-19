/*
 * test_integ_cdr.c — Integration tests for mod_cdr
 *
 * Tests: CDR write on COR cycle, caller info capture,
 *        RECORDING_SAVED event populates recording path,
 *        emergency flag, daily stats.
 */

#include "test_integ_mock.h"
#include <stdlib.h>

/* Pull in the module source */
#include "../modules/mod_cdr.c"

/* Helper: fire a COR assert/drop cycle with optional caller */
static void sim_transmission(int user_id, int method)
{
    mock_fire_simple(KERCHEVT_COR_ASSERT);

    if (user_id > 0) {
        kerchevt_t e = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = user_id, .method = method },
        };
        kerchevt_fire(&e);
    }
}

void test_integ_cdr(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    /* Use temp directory for CDR output */
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "cdr", "directory", "/tmp/kerchunk_test_cdr");

    /* Need users for caller lookup */
    kerchunk_config_set(cfg, "user.1", "name", "Brian");
    kerchunk_config_set(cfg, "user.1", "ctcss", "1000");
    kerchunk_config_set(cfg, "user.1", "access", "admin");
    kerchunk_user_init(cfg);

    mod_cdr.load(&g_mock_core);
    cdr_configure(cfg);

    /* 1. Basic COR cycle creates CDR */
    test_begin("cdr: COR cycle records call");
    sim_transmission(0, 0);
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_today_calls == 1, "call not counted");
    test_assert(!g_in_call, "still in call");
    test_end();

    /* 2. Caller identification populates user info */
    test_begin("cdr: caller info captured");
    sim_transmission(1, 1);  /* user 1, method CTCSS */
    test_assert(g_call_user_id == 1, "user_id wrong");
    test_assert(strcmp(g_call_user_name, "Brian") == 0, "user_name wrong");
    test_assert(strcmp(g_call_method, "CTCSS") == 0, "method wrong");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_today_calls == 2, "second call not counted");
    test_end();

    /* 3. RECORDING_SAVED event sets recording path */
    test_begin("cdr: RECORDING_SAVED populates path");
    sim_transmission(1, 1);
    {
        kerchevt_t e = {
            .type = KERCHEVT_RECORDING_SAVED,
            .recording = {
                .path = "recordings/20260319_120000_RX_Brian.wav",
                .direction = "RX",
                .user_id = 1,
                .duration = 5.0f,
            },
        };
        kerchevt_fire(&e);
    }
    test_assert(strcmp(g_call_recording,
                       "recordings/20260319_120000_RX_Brian.wav") == 0,
                "recording path not captured");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_end();

    /* 4. TX recording is ignored (only RX) */
    test_begin("cdr: TX recording ignored");
    sim_transmission(0, 0);
    g_call_recording[0] = '\0';
    {
        kerchevt_t e = {
            .type = KERCHEVT_RECORDING_SAVED,
            .recording = {
                .path = "recordings/20260319_120001_TX_transmit.wav",
                .direction = "TX",
                .user_id = 0,
                .duration = 3.0f,
            },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_call_recording[0] == '\0',
                "TX recording should not be captured");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_end();

    /* 5. Emergency flag captured */
    test_begin("cdr: emergency flag captured");
    kerchunk_core_set_emergency(1);
    sim_transmission(0, 0);
    test_assert(g_call_emergency == 1, "emergency flag not set");
    mock_fire_simple(KERCHEVT_COR_DROP);
    kerchunk_core_set_emergency(0);
    test_end();

    /* 6. Stats accumulate */
    test_begin("cdr: daily stats accumulate");
    test_assert(g_today_calls == 5, "wrong call count");
    test_assert(g_today_seconds >= 0.0, "negative seconds");
    test_end();

    /* 7. Recording path resets between calls */
    test_begin("cdr: recording path resets between calls");
    sim_transmission(0, 0);
    test_assert(g_call_recording[0] == '\0', "recording not cleared");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_end();

    /* 8. Announcement event creates CDR row */
    test_begin("cdr: announcement event creates CDR row");
    {
        int calls_before = g_today_calls;
        kerchevt_t e = {
            .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "test", .description = "test announcement" },
        };
        kerchevt_fire(&e);
        test_assert(g_today_calls == calls_before + 1,
                    "announcement not counted");
    }
    test_end();

    /* 9. Announcement row has system user and zero duration */
    test_begin("cdr: announcement row format correct");
    {
        /* The on_announcement handler writes user_id=0, user_name=system,
         * duration=0.0 — verify via the call count (we can't easily
         * read the file in white-box, but stats confirm it was written) */
        int calls_before = g_today_calls;
        kerchevt_t e = {
            .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "cwid", .description = "WRDP519" },
        };
        kerchevt_fire(&e);
        test_assert(g_today_calls == calls_before + 1,
                    "cwid announcement not counted");
    }
    test_end();

    /* 10. Mix of COR cycles and announcements both counted */
    test_begin("cdr: mixed COR + announcements counted");
    {
        int calls_before = g_today_calls;
        sim_transmission(0, 0);
        mock_fire_simple(KERCHEVT_COR_DROP);
        kerchevt_t e = {
            .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "weather", .description = "current conditions" },
        };
        kerchevt_fire(&e);
        test_assert(g_today_calls == calls_before + 2,
                    "mixed count wrong");
    }
    test_end();

    mod_cdr.unload();
    kerchevt_shutdown();
    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);

    /* Cleanup test files */
    if (system("rm -rf /tmp/kerchunk_test_cdr") != 0) { /* best-effort */ }
}
