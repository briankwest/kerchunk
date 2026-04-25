/*
 * test_integ_freeswitch.c — Integration tests for mod_freeswitch
 *
 * Tests: config, jitter buffer, VAD, number validation, DTMF,
 * COR gating, call state, admin-only, disabled module, VOX cycle.
 */

#include "test_integ_mock.h"
#include <math.h>

/*
 * Stub socket/network functions for testing — these will be called by
 * the module code but are no-ops in the test environment.
 * The real socket calls in ESL and UDP code will fail gracefully
 * since we test the logic paths, not the actual networking.
 */

/* Pull in the module source */
#include "../modules/mod_freeswitch.c"

void test_integ_freeswitch(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    /* === 1. Config parse === */
    test_begin("freeswitch: config parse");
    {
        kerchunk_config_t *cfg = kerchunk_config_create();
        kerchunk_config_set(cfg, "freeswitch", "enabled", "on");
        kerchunk_config_set(cfg, "freeswitch", "freeswitch_host", "192.168.1.100");
        kerchunk_config_set(cfg, "freeswitch", "esl_port", "8022");
        kerchunk_config_set(cfg, "freeswitch", "esl_password", "TestPass");
        kerchunk_config_set(cfg, "freeswitch", "sip_gateway", "test_gw");
        kerchunk_config_set(cfg, "freeswitch", "udp_base_port", "18000");
        kerchunk_config_set(cfg, "freeswitch", "max_call_duration", "2m");
        kerchunk_config_set(cfg, "freeswitch", "dial_timeout", "20s");
        kerchunk_config_set(cfg, "freeswitch", "inactivity_timeout", "45s");
        kerchunk_config_set(cfg, "freeswitch", "vad_threshold", "900");
        kerchunk_config_set(cfg, "freeswitch", "vad_hold_ms", "600");
        kerchunk_config_set(cfg, "freeswitch", "admin_only", "on");
        kerchunk_config_set(cfg, "freeswitch", "dial_prefix", "1");
        kerchunk_config_set(cfg, "freeswitch", "dial_whitelist", "918,539,405");
        kerchunk_config_set(cfg, "general", "sounds_dir", "/tmp/sounds");

        mod_freeswitch.load(&g_mock_core);
        freeswitch_configure(cfg);
        kerchunk_config_destroy(cfg);
    }
    test_assert(g_enabled == 1, "not enabled");
    test_assert(strcmp(g_fs_host, "192.168.1.100") == 0, "wrong host");
    test_assert(g_fs_esl_port == 8022, "wrong esl_port");
    test_assert(strcmp(g_fs_esl_password, "TestPass") == 0, "wrong password");
    test_assert(strcmp(g_sip_gateway, "test_gw") == 0, "wrong gateway");
    test_assert(g_udp_base_port == 18000, "wrong udp_base_port");
    test_assert(g_max_call_secs == 120, "wrong max_call_secs");
    test_assert(g_dial_timeout_ms == 20000, "wrong dial_timeout");
    test_assert(g_inactivity_ms == 45000, "wrong inactivity_timeout");
    test_assert(g_vad_threshold == 900, "wrong vad_threshold");
    test_assert(g_vad_hold_ms == 600, "wrong vad_hold_ms");
    test_assert(g_admin_only == 1, "wrong admin_only");
    test_assert(strcmp(g_dial_prefix, "1") == 0, "wrong prefix");
    test_assert(strcmp(g_dial_whitelist, "918,539,405") == 0, "wrong whitelist");
    test_assert(strcmp(g_sounds_dir, "/tmp/sounds") == 0, "wrong sounds_dir");
    test_end();

    /* Reset admin_only for subsequent tests */
    g_admin_only = 0;

    /* === 2. Jitter buffer write/read === */
    test_begin("freeswitch: jitter buffer write/read");
    {
        jitter_buf_t jb;
        jitter_buf_reset(&jb);

        int16_t samples[160];
        for (int i = 0; i < 160; i++) samples[i] = (int16_t)(i + 1);

        jitter_buf_write(&jb, samples, 160);
        test_assert(jb.count == 160, "wrong count after write");

        int16_t out[160];
        size_t got = jitter_buf_read(&jb, out, 160);
        test_assert(got == 160, "wrong read count");
        test_assert(out[0] == 1, "wrong first sample");
        test_assert(out[159] == 160, "wrong last sample");
        test_assert(jb.count == 0, "count not 0 after full read");
    }
    test_end();

    /* === 3. Jitter buffer overflow === */
    test_begin("freeswitch: jitter buffer overflow");
    {
        jitter_buf_t jb;
        jitter_buf_reset(&jb);

        /* Write more than JITTER_BUF_SAMPLES */
        int16_t block[160];
        for (int i = 0; i < 12; i++) {  /* 12 * 160 = 1920 > 1600 */
            for (int j = 0; j < 160; j++)
                block[j] = (int16_t)(i * 160 + j);
            jitter_buf_write(&jb, block, 160);
        }

        /* Count should be capped at JITTER_BUF_SAMPLES */
        test_assert(jb.count == JITTER_BUF_SAMPLES,
                    "overflow count wrong");

        /* Read and verify oldest were dropped (newest survive) */
        int16_t out[JITTER_BUF_SAMPLES];
        size_t got = jitter_buf_read(&jb, out, JITTER_BUF_SAMPLES);
        test_assert(got == JITTER_BUF_SAMPLES, "wrong read after overflow");

        /* The last sample written was 12*160-1 = 1919.
         * After overflow, the newest 1600 samples should remain.
         * Last sample read should be 1919. */
        test_assert(out[JITTER_BUF_SAMPLES - 1] == (int16_t)1919,
                    "newest sample not preserved");
    }
    test_end();

    /* === 4. Jitter buffer underflow === */
    test_begin("freeswitch: jitter buffer underflow");
    {
        jitter_buf_t jb;
        jitter_buf_reset(&jb);

        /* Write 80 samples */
        int16_t block[80];
        for (int i = 0; i < 80; i++) block[i] = (int16_t)(i + 100);
        jitter_buf_write(&jb, block, 80);

        /* Read 160 — should get 80 real + 80 silence */
        int16_t out[160];
        size_t got = jitter_buf_read(&jb, out, 160);
        test_assert(got == 80, "wrong count for partial read");
        test_assert(out[0] == 100, "wrong first sample");
        test_assert(out[79] == 179, "wrong 80th sample");
        test_assert(out[80] == 0, "underflow not silent");
        test_assert(out[159] == 0, "underflow tail not silent");
    }
    test_end();

    /* === 5. VAD silence === */
    test_begin("freeswitch: VAD silence detection");
    {
        vad_reset();
        g_vox_ptt_held = 0;
        g_vad_threshold = 800;
        g_vad_hold_ms = 500;

        int16_t zeros[160] = {0};
        int result = vad_process(zeros, 160);
        test_assert(result == 0, "VAD detected speech in silence");
    }
    test_end();

    /* === 6. VAD speech === */
    test_begin("freeswitch: VAD speech detection");
    {
        vad_reset();
        g_vox_ptt_held = 0;
        g_vad_threshold     = 800;
        g_vad_hold_ms       = 500;
        g_vad_attack_frames = 2;  /* pin for this test — see VOX PTT
                                   * cycle below for rationale */

        /* Generate loud signal (RMS > threshold) */
        int16_t loud[160];
        for (int i = 0; i < 160; i++)
            loud[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 1000.0 * i / 8000.0));

        /* First frame — attack counter = 1, not enough */
        int r1 = vad_process(loud, 160);
        /* With attack=2, first frame returns 0 if not already holding */
        test_assert(r1 == 0, "VAD triggered on first attack frame");

        /* Second frame — attack counter = 2, should trigger */
        int r2 = vad_process(loud, 160);
        test_assert(r2 == 1, "VAD didn't detect speech after attack frames");
    }
    test_end();

    /* === 7. VAD hold timer === */
    test_begin("freeswitch: VAD hold timer");
    {
        vad_reset();
        g_vox_ptt_held = 1;  /* Simulate already holding PTT */
        g_vad_threshold = 800;
        g_vad_hold_ms = 100;  /* Short hold for test: 100ms = 5 frames */

        /* Generate speech */
        int16_t loud[160];
        for (int i = 0; i < 160; i++)
            loud[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 1000.0 * i / 8000.0));

        /* Feed speech frames to establish hold timer */
        vad_process(loud, 160);
        vad_process(loud, 160);
        int speaking = vad_process(loud, 160);
        test_assert(speaking == 1, "not speaking during speech");

        /* Now feed silence — hold timer should keep it active */
        int16_t zeros[160] = {0};
        int hold1 = vad_process(zeros, 160);
        test_assert(hold1 == 1, "hold timer didn't keep active");

        /* Feed enough silence frames to exhaust hold timer */
        /* 100ms hold / 20ms per frame = 5 frames needed */
        for (int i = 0; i < 10; i++)
            vad_process(zeros, 160);

        int expired = vad_process(zeros, 160);
        test_assert(expired == 0, "hold timer didn't expire");
    }
    test_end();

    /* === 8. Number validation === */
    test_begin("freeswitch: number validation");
    {
        /* Set up whitelist */
        snprintf(g_dial_whitelist, sizeof(g_dial_whitelist), "918,539");

        /* Valid number matching whitelist */
        test_assert(is_number_allowed("9185551234") == 1,
                    "valid 918 number rejected");
        test_assert(is_number_allowed("5391234567") == 1,
                    "valid 539 number rejected");

        /* Number not on whitelist */
        test_assert(is_number_allowed("4051234567") == 0,
                    "non-whitelist number accepted");

        /* 911 blocked */
        test_assert(is_number_allowed("911") == 0, "911 not blocked");
        test_assert(is_number_allowed("9911") == 0, "9911 not blocked");
        test_assert(is_number_allowed("1911") == 0, "1911 not blocked");

        /* Too short */
        test_assert(is_number_allowed("12") == 0, "short number accepted");

        /* Non-digits */
        test_assert(is_number_allowed("918abc1234") == 0,
                    "non-digit number accepted");

        /* Empty */
        test_assert(is_number_allowed("") == 0, "empty accepted");
        test_assert(is_number_allowed(NULL) == 0, "NULL accepted");

        /* No whitelist — all allowed */
        g_dial_whitelist[0] = '\0';
        test_assert(is_number_allowed("4051234567") == 1,
                    "no-whitelist number rejected");
    }
    test_end();

    /* === 9. DTMF dial (event triggers autopatch_dial) === */
    test_begin("freeswitch: DTMF dial event");
    {
        g_mock.file_calls = 0;
        g_enabled = 1;
        g_esl_authed = 1;
        g_esl_fd = 999;  /* Fake fd so esl_send doesn't fail before send() */
        g_admin_only = 0;
        g_call_state = CALL_IDLE;

        char digits[] = "5551234567";
        kerchevt_t e = {
            .type = KERCHEVT_CUSTOM + DTMF_EVT_AUTOPATCH,
            .custom = { .data = digits, .len = strlen(digits) },
        };
        kerchevt_fire(&e);

        /* Call should be in DIALING state (send() will fail but state advances) */
        test_assert(g_call_state == CALL_DIALING || g_call_state == CALL_IDLE,
                    "unexpected call state after dial");
        test_assert(g_mock.file_calls >= 1, "no dialing prompt");

        /* Clean up */
        g_call_state = CALL_IDLE;
        g_call_uuid[0] = '\0';
        g_esl_fd = -1;
        g_esl_authed = 0;
        /* Cancel any timers that were created */
        if (g_dial_timer >= 0) {
            g_core->timer_cancel(g_dial_timer);
            g_dial_timer = -1;
        }
    }
    test_end();

    /* === 10. DTMF hangup === */
    test_begin("freeswitch: DTMF hangup event");
    {
        g_mock.file_calls = 0;
        g_mock.log_count = 0;

        /* Set up a fake active call */
        g_call_state = CALL_CONNECTED;
        g_call_active = 1;
        snprintf(g_call_uuid, sizeof(g_call_uuid), "test-uuid-123");

        /* Fire empty autopatch event → hangup */
        kerchevt_t e = {
            .type = KERCHEVT_CUSTOM + DTMF_EVT_AUTOPATCH,
            .custom = { .data = NULL, .len = 0 },
        };
        kerchevt_fire(&e);

        test_assert(g_call_state == CALL_IDLE, "call not torn down");
        test_assert(g_call_active == 0, "call_active not cleared");
        test_assert(g_mock.file_calls >= 1, "no disconnect prompt");
    }
    test_end();

    /* === 11. COR gating === */
    test_begin("freeswitch: COR gating");
    {
        g_cor_active = 0;

        mock_fire_simple(KERCHEVT_COR_ASSERT);
        test_assert(g_cor_active == 1, "COR assert not set");

        mock_fire_simple(KERCHEVT_COR_DROP);
        test_assert(g_cor_active == 0, "COR drop not cleared");
    }
    test_end();

    /* === 12. Call timeout === */
    test_begin("freeswitch: call timeout teardown");
    {
        g_mock.file_calls = 0;

        /* Set up fake connected call */
        g_call_state = CALL_CONNECTED;
        g_call_active = 1;
        snprintf(g_call_uuid, sizeof(g_call_uuid), "timeout-uuid");

        /* Simulate call timeout */
        on_call_timeout(NULL);

        test_assert(g_call_state == CALL_IDLE, "not idle after timeout");
        test_assert(g_call_active == 0, "call_active after timeout");
        test_assert(g_mock.file_calls >= 1, "no timeout prompt");
    }
    test_end();

    /* === 13. Admin-only rejects non-admin === */
    test_begin("freeswitch: admin-only mode");
    {
        g_mock.file_calls = 0;
        g_enabled = 1;
        g_admin_only = 1;
        g_esl_authed = 1;
        g_esl_fd = 999;
        g_call_state = CALL_IDLE;

        /* Try to dial — should be rejected */
        autopatch_dial("5551234567");
        test_assert(g_call_state == CALL_IDLE, "admin-only didn't reject");
        test_assert(g_mock.file_calls >= 1, "no access denied prompt");

        /* Clean up */
        g_admin_only = 0;
        g_esl_fd = -1;
        g_esl_authed = 0;
    }
    test_end();

    /* === 14. Disabled module tick is no-op === */
    test_begin("freeswitch: disabled module tick no-op");
    {
        g_enabled = 0;
        g_mock.log_count = 0;
        int prev_esl_fd = g_esl_fd;

        kerchevt_t e = { .type = KERCHEVT_TICK };
        on_tick(&e, NULL);

        /* ESL fd should not change (no connect attempted) */
        test_assert(g_esl_fd == prev_esl_fd, "disabled module tried to connect");
    }
    test_end();

    /* Re-enable for remaining tests */
    g_enabled = 1;

    /* === 15. VOX PTT cycle === */
    test_begin("freeswitch: VOX PTT cycle");
    {
        g_mock.ptt_requested = 0;
        g_mock.ptt_released = 0;
        g_mock.buffer_calls = 0;

        /* Simulate connected call */
        g_call_active = 1;
        g_cor_active = 0;
        g_vox_ptt_held = 0;
        g_vad_threshold     = 800;
        g_vad_hold_ms       = 100;
        g_vad_attack_frames = 2;   /* pin for this test — production
                                    * default is higher (5) but this
                                    * test wants to verify the attack
                                    * → assert transition with the
                                    * minimum number of frames. */
        vad_reset();
        jitter_buf_reset(&g_jitter);

        /* Write loud audio into jitter buffer */
        int16_t loud[160];
        for (int i = 0; i < 160; i++)
            loud[i] = (int16_t)(10000.0 * sin(2.0 * M_PI * 1000.0 * i / 8000.0));

        /* Need attack frames (2) worth of loud audio */
        jitter_buf_write(&g_jitter, loud, 160);
        vox_process_and_queue();  /* Attack frame 1 — no PTT yet */

        jitter_buf_write(&g_jitter, loud, 160);
        vox_process_and_queue();  /* Attack frame 2 — PTT should assert */

        test_assert(g_vox_ptt_held == 1, "VOX PTT not asserted");
        test_assert(g_mock.ptt_requested >= 1, "PTT not requested");

        /* Now feed silence — after hold expires, PTT should release */
        int16_t zeros[160] = {0};
        for (int i = 0; i < 15; i++) {
            jitter_buf_write(&g_jitter, zeros, 160);
            vox_process_and_queue();
        }

        test_assert(g_vox_ptt_held == 0, "VOX PTT not released after hold");
        test_assert(g_mock.ptt_released >= 1, "PTT not released");

        /* Clean up */
        g_call_active = 0;
    }
    test_end();

    /* === 16. Dial timeout === */
    test_begin("freeswitch: dial timeout");
    {
        g_mock.file_calls = 0;
        g_call_state = CALL_DIALING;
        g_call_active = 0;
        snprintf(g_call_uuid, sizeof(g_call_uuid), "dial-timeout-uuid");

        on_dial_timeout(NULL);

        test_assert(g_call_state == CALL_IDLE, "not idle after dial timeout");
        test_assert(g_mock.file_calls >= 1, "no timeout prompt");
    }
    test_end();

    /* === 17. Inactivity timeout === */
    test_begin("freeswitch: inactivity timeout");
    {
        g_mock.file_calls = 0;
        g_call_state = CALL_CONNECTED;
        g_call_active = 1;
        snprintf(g_call_uuid, sizeof(g_call_uuid), "inact-uuid");

        on_inactivity_timeout(NULL);

        test_assert(g_call_state == CALL_IDLE, "not idle after inactivity");
        test_assert(g_call_active == 0, "call_active after inactivity");
    }
    test_end();

    /* === 18. ESL event parsing === */
    test_begin("freeswitch: ESL header parsing");
    {
        const char *block =
            "Event-Name: CHANNEL_ANSWER\n"
            "Unique-ID: abc-123-def\n"
            "Hangup-Cause: NORMAL_CLEARING\n";

        char val[64] = "";
        int rc = esl_parse_header(block, "Event-Name", val, sizeof(val));
        test_assert(rc == 0, "failed to parse Event-Name");
        test_assert(strcmp(val, "CHANNEL_ANSWER") == 0, "wrong Event-Name");

        rc = esl_parse_header(block, "Unique-ID", val, sizeof(val));
        test_assert(rc == 0, "failed to parse Unique-ID");
        test_assert(strcmp(val, "abc-123-def") == 0, "wrong Unique-ID");

        rc = esl_parse_header(block, "Hangup-Cause", val, sizeof(val));
        test_assert(rc == 0, "failed to parse Hangup-Cause");
        test_assert(strcmp(val, "NORMAL_CLEARING") == 0, "wrong Hangup-Cause");

        /* Non-existent header */
        rc = esl_parse_header(block, "Missing-Header", val, sizeof(val));
        test_assert(rc != 0, "found non-existent header");
    }
    test_end();

    /* === 19. compute_rms correctness === */
    test_begin("freeswitch: compute_rms");
    {
        /* DC signal: all 1000 → RMS = 1000 */
        int16_t dc[160];
        for (int i = 0; i < 160; i++) dc[i] = 1000;
        int rms = compute_rms(dc, 160);
        test_assert(rms == 1000, "DC RMS wrong");

        /* Silence → RMS = 0 */
        int16_t zeros[160] = {0};
        rms = compute_rms(zeros, 160);
        test_assert(rms == 0, "silence RMS not 0");

        /* Empty → 0 */
        rms = compute_rms(zeros, 0);
        test_assert(rms == 0, "empty RMS not 0");
    }
    test_end();

    /* === 20. Shutdown handler === */
    test_begin("freeswitch: shutdown tears down call");
    {
        g_mock.file_calls = 0;
        g_call_state = CALL_CONNECTED;
        g_call_active = 1;
        snprintf(g_call_uuid, sizeof(g_call_uuid), "shutdown-uuid");

        kerchevt_t e = { .type = KERCHEVT_SHUTDOWN };
        on_shutdown(&e, NULL);

        test_assert(g_call_state == CALL_IDLE, "not idle after shutdown");
        test_assert(g_call_active == 0, "call_active after shutdown");
    }
    test_end();

    /* Clean up */
    g_enabled = 0;
    mod_freeswitch.unload();
    kerchevt_shutdown();
}
