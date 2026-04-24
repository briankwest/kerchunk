/*
 * test_integ_repeater.c — Integration tests for mod_repeater state machine
 *
 * Includes mod_repeater.c directly (KERCHUNK_MODULE_DEFINE suppressed).
 * Exercises the full IDLE→RECEIVING→TAIL_WAIT→HANG_WAIT→IDLE cycle,
 * re-key, TOT, and timeout recovery.
 */

#include "test_integ_mock.h"

/* Pull in the module source (statics become local to this TU) */
#include "../modules/mod_repeater.c"

/* ---- tracking handlers ---- */

static int t_last_old = -1, t_last_new = -1;
static int t_state_changes;
static int t_tail_started;
static int t_tail_expired;
static int t_timeout_count;

static void t_state_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    t_last_old = evt->state.old_state;
    t_last_new = evt->state.new_state;
    t_state_changes++;
}
static void t_tail_handler(const kerchevt_t *e, void *u) { (void)e; (void)u; t_tail_started++; }
static void t_expire_handler(const kerchevt_t *e, void *u) { (void)e; (void)u; t_tail_expired++; }
static void t_timeout_handler(const kerchevt_t *e, void *u) { (void)e; (void)u; t_timeout_count++; }

static void reset_track(void)
{
    t_last_old = t_last_new = -1;
    t_state_changes = 0;
    t_tail_started = 0;
    t_tail_expired = 0;
    t_timeout_count = 0;
}

/* ---- entry point ---- */

void test_integ_repeater(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    kerchevt_subscribe(KERCHEVT_RX_STATE_CHANGE, t_state_handler, NULL);
    kerchevt_subscribe(KERCHEVT_TAIL_START,   t_tail_handler, NULL);
    kerchevt_subscribe(KERCHEVT_TAIL_EXPIRE,  t_expire_handler, NULL);
    kerchevt_subscribe(KERCHEVT_RX_TIMEOUT,      t_timeout_handler, NULL);

    mod_repeater.load(&g_mock_core);

    /* Enable software relay so PTT is asserted on COR (tests expect this) */
    g_software_relay = 1;
    /* Disable debounce for legacy tests */
    g_cor_debounce_ms = 0;

    /* 1. COR assert: IDLE → RECEIVING */
    test_begin("repeater: IDLE → RECEIVING on COR");
    reset_track();
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_state == RPT_RECEIVING, "wrong state");
    test_assert(g_mock.ptt_requested >= 1, "PTT not requested");
    test_assert(g_tot_timer >= 0, "TOT timer not started");
    test_assert(t_last_new == RPT_RECEIVING, "state event wrong");
    test_end();

    /* 2. COR drop: RECEIVING → TAIL_WAIT */
    test_begin("repeater: RECEIVING → TAIL_WAIT on COR drop");
    reset_track();
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_state == RPT_TAIL_WAIT, "wrong state");
    test_assert(g_tail_timer >= 0, "tail timer not started");
    test_assert(t_tail_started == 1, "TAIL_START not fired");
    test_end();

    /* 3. tail expire: TAIL_WAIT → HANG_WAIT */
    test_begin("repeater: TAIL_WAIT → HANG_WAIT on tail expire");
    reset_track();
    mock_fire_timer(g_tail_timer);
    test_assert(g_state == RPT_HANG_WAIT, "wrong state");
    test_assert(t_tail_expired == 1, "TAIL_EXPIRE not fired");
    test_assert(g_hang_timer >= 0, "hang timer not started");
    test_end();

    /* 4. hang expire: HANG_WAIT → IDLE */
    test_begin("repeater: HANG_WAIT → IDLE on hang expire");
    reset_track();
    mock_fire_timer(g_hang_timer);
    test_assert(g_state == RPT_IDLE, "wrong state");
    test_assert(g_mock.ptt_released >= 1, "PTT not released");
    test_end();

    /* 5. re-key during TAIL_WAIT cancels tail timer */
    test_begin("repeater: rekey during TAIL_WAIT");
    mock_fire_simple(KERCHEVT_COR_ASSERT);           /* IDLE → RECEIVING */
    mock_fire_simple(KERCHEVT_COR_DROP);              /* → TAIL_WAIT */
    test_assert(g_state == RPT_TAIL_WAIT, "setup");
    int old_tail = g_tail_timer;
    mock_fire_simple(KERCHEVT_COR_ASSERT);            /* rekey */
    test_assert(g_state == RPT_RECEIVING, "not RECEIVING");
    test_assert(!mock_timer_active(old_tail), "tail timer not cancelled");
    test_end();

    /* 6. re-key during HANG_WAIT cancels hang timer, restarts TOT */
    test_begin("repeater: rekey during HANG_WAIT");
    mock_fire_simple(KERCHEVT_COR_DROP);              /* → TAIL_WAIT */
    mock_fire_timer(g_tail_timer);                   /* → HANG_WAIT */
    test_assert(g_state == RPT_HANG_WAIT, "setup");
    int old_hang = g_hang_timer;
    mock_fire_simple(KERCHEVT_COR_ASSERT);            /* rekey */
    test_assert(g_state == RPT_RECEIVING, "not RECEIVING");
    test_assert(!mock_timer_active(old_hang), "hang timer not cancelled");
    test_assert(g_tot_timer >= 0, "TOT not restarted");
    test_end();

    /* 7. TOT fires warning tone */
    test_begin("repeater: TOT fires warning tone");
    reset_track();
    g_mock.tone_calls = 0;
    test_assert(g_state == RPT_RECEIVING, "setup");
    mock_fire_timer(g_tot_timer);
    test_assert(g_state == RPT_RX_TIMEOUT, "not TIMEOUT");
    test_assert(g_mock.tone_calls >= 1, "no warning tone");
    test_assert(t_timeout_count == 1, "TIMEOUT event not fired");
    test_end();

    /* 8. COR assert ignored during TIMEOUT */
    test_begin("repeater: COR ignored during TIMEOUT");
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_state == RPT_RX_TIMEOUT, "state changed");
    test_end();

    /* 9. COR drop during TIMEOUT → IDLE */
    test_begin("repeater: TIMEOUT → IDLE on COR drop");
    g_mock.ptt_released = 0;
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_state == RPT_IDLE, "not IDLE");
    test_assert(g_mock.ptt_released >= 1, "PTT not released");
    test_end();

    /* ── Kerchunk filter tests ── */

    /* Reset to IDLE for kerchunk tests */
    g_state = RPT_IDLE;
    g_cor_debounce_ms = 150;

    /* 10. Kerchunk filtered: COR assert + drop before debounce → stays IDLE */
    test_begin("repeater: kerchunk filtered (COR too short)");
    reset_track();
    g_mock.ptt_requested = 0;
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_state == RPT_IDLE, "should still be IDLE during debounce");
    test_assert(g_debounce_timer >= 0, "debounce timer not started");
    test_assert(g_mock.ptt_requested == 0, "PTT should not be requested yet");
    /* COR drops before timer fires → kerchunk */
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_state == RPT_IDLE, "not IDLE after kerchunk");
    test_assert(g_debounce_timer == -1, "debounce timer not cancelled");
    test_end();

    /* 11. Debounce expires → transition to RECEIVING */
    test_begin("repeater: debounce expires → RECEIVING");
    reset_track();
    g_mock.ptt_requested = 0;
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_debounce_timer >= 0, "no debounce timer");
    int dbt = g_debounce_timer;
    mock_fire_timer(dbt);
    test_assert(g_state == RPT_RECEIVING, "not RECEIVING after debounce");
    test_assert(g_mock.ptt_requested >= 1, "PTT not requested");
    test_end();

    /* Clean up: COR drop back to IDLE */
    mock_fire_simple(KERCHEVT_COR_DROP);

    /* 12. Re-key during TAIL_WAIT: no debounce needed */
    test_begin("repeater: rekey from TAIL_WAIT, no debounce");
    reset_track();
    g_mock.ptt_requested = 0;
    /* Get to TAIL_WAIT: COR assert (with debounce) → debounce fire → COR drop */
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    mock_fire_timer(g_debounce_timer);
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_state == RPT_TAIL_WAIT, "setup failed");
    /* Re-key goes directly to RECEIVING (no debounce from TAIL_WAIT) */
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_state == RPT_RECEIVING, "not RECEIVING after rekey");
    test_end();

    /* Clean up */
    mock_fire_simple(KERCHEVT_COR_DROP);
    /* Let tail + hang expire to get back to IDLE */
    if (g_tail_timer >= 0)
        mock_fire_timer(g_tail_timer);
    if (g_hang_timer >= 0)
        mock_fire_timer(g_hang_timer);

    mod_repeater.unload();

    /* ════════════════════════════════════════════════════════════════
     *  Closed repeater tests (require_identification = on)
     * ════════════════════════════════════════════════════════════════ */

    kerchevt_init();
    mock_reset();
    mock_init_core();

    kerchevt_subscribe(KERCHEVT_RX_STATE_CHANGE, t_state_handler, NULL);

    kerchunk_config_t *cfg2 = kerchunk_config_create();
    kerchunk_config_set(cfg2, "repeater", "require_identification", "on");
    kerchunk_config_set(cfg2, "repeater", "cor_debounce", "100");
    kerchunk_config_set(cfg2, "repeater", "software_relay", "on");

    mod_repeater.load(&g_mock_core);
    repeater_configure(cfg2);

    /* 13. Closed repeater: COR without ID → stays IDLE after debounce */
    test_begin("repeater: closed — no ID, access denied");
    reset_track();
    g_mock.ptt_requested = 0;
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_debounce_timer >= 0, "no debounce timer");
    /* Debounce fires — no caller identified */
    mock_fire_timer(g_debounce_timer);
    test_assert(g_state == RPT_IDLE, "should stay IDLE");
    test_assert(g_mock.ptt_requested == 0, "PTT should not be requested");
    /* Access denied tone or TTS queued */
    test_assert(g_mock.tone_calls > 0 || g_mock.buffer_calls > 0,
                "no access denied indication");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_end();

    /* 14. Closed repeater: COR with ID during debounce → RECEIVING */
    test_begin("repeater: closed — ID during debounce, opens");
    reset_track();
    g_mock.ptt_requested = 0;
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_debounce_timer >= 0, "no debounce timer");

    /* Caller identified during debounce */
    {
        kerchevt_t id = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = 1, .method = 1 },
        };
        kerchevt_fire(&id);
    }
    test_assert(g_caller_identified == 1, "caller not flagged");

    /* Debounce fires — caller already identified */
    mock_fire_timer(g_debounce_timer);
    test_assert(g_state == RPT_RECEIVING, "should be RECEIVING");
    test_assert(g_mock.ptt_requested >= 1, "PTT not requested");
    mock_fire_simple(KERCHEVT_COR_DROP);
    /* Clean up */
    if (g_tail_timer >= 0) mock_fire_timer(g_tail_timer);
    if (g_hang_timer >= 0) mock_fire_timer(g_hang_timer);
    test_end();

    /* 15. Closed repeater: kerchunk during debounce → IDLE */
    test_begin("repeater: closed — kerchunk filtered");
    reset_track();
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    mock_fire_simple(KERCHEVT_COR_DROP);  /* Drop before debounce */
    test_assert(g_state == RPT_IDLE, "not IDLE after kerchunk");
    test_assert(g_debounce_timer == -1, "debounce not cancelled");
    test_end();

    /* 16. Closed repeater: rekey from TAIL_WAIT skips ID check */
    test_begin("repeater: closed — rekey from tail, no re-ID needed");
    reset_track();
    g_mock.ptt_requested = 0;
    /* Get to RECEIVING with ID */
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    {
        kerchevt_t id = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = 1, .method = 1 },
        };
        kerchevt_fire(&id);
    }
    mock_fire_timer(g_debounce_timer);
    test_assert(g_state == RPT_RECEIVING, "setup: not RECEIVING");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_state == RPT_TAIL_WAIT, "setup: not TAIL_WAIT");
    /* Rekey — should go straight to RECEIVING (no debounce) */
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_state == RPT_RECEIVING, "rekey failed");
    /* Clean up */
    mock_fire_simple(KERCHEVT_COR_DROP);
    if (g_tail_timer >= 0) mock_fire_timer(g_tail_timer);
    if (g_hang_timer >= 0) mock_fire_timer(g_hang_timer);
    test_end();

    /* 17. Minimum debounce enforced (500ms) when require_id is on */
    test_begin("repeater: closed — min 500ms debounce enforced");
    test_assert(g_require_id == 1, "require_id not on");
    /* cor_debounce was set to 100ms, but require_id enforces min 500ms */
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(g_debounce_timer >= 0, "no debounce timer");
    /* Check the timer was created with at least 500ms */
    for (int i = 0; i < MOCK_MAX_TIMERS; i++) {
        if (g_mock.timers[i].active && g_mock.timers[i].id == g_debounce_timer) {
            test_assert(g_mock.timers[i].ms >= 500, "debounce < 500ms");
            break;
        }
    }
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_end();

    mod_repeater.unload();
    kerchevt_unsubscribe(KERCHEVT_RX_STATE_CHANGE, t_state_handler);
    kerchevt_shutdown();
    kerchunk_config_destroy(cfg2);
}
