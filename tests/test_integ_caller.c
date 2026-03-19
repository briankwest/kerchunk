/*
 * test_integ_caller.c — Integration tests for caller identification
 *
 * Includes mod_caller.c directly.
 * Tests ANI capture, DTMF login, session persistence, clear on COR drop.
 *
 * Note: CTCSS/DCS identification has been removed. Caller ID uses
 * DTMF ANI and DTMF login only.
 */

#include "test_integ_mock.h"

/* Pull in the module source */
#include "../modules/mod_caller.c"

/* ---- event tracking ---- */

static int t_caller_id;
static int t_caller_method;
static int t_caller_cleared;

static void t_id_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    t_caller_id     = evt->caller.user_id;
    t_caller_method = evt->caller.method;
}

static void t_clear_handler(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    t_caller_cleared = 1;
}

static void reset_caller_track(void)
{
    t_caller_id      = 0;
    t_caller_method  = 0;
    t_caller_cleared = 0;
}

/* ---- entry point ---- */

void test_integ_caller(void)
{
    /* Set up user database */
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "user.1", "name",       "Brian");
    kerchunk_config_set(cfg, "user.1", "dtmf_login", "101");
    kerchunk_config_set(cfg, "user.1", "ani",         "5551");
    kerchunk_config_set(cfg, "user.1", "access",      "2");
    kerchunk_config_set(cfg, "user.2", "name",       "Alice");
    kerchunk_config_set(cfg, "user.2", "dtmf_login", "102");
    kerchunk_config_set(cfg, "user.2", "access",      "1");
    kerchunk_config_set(cfg, "caller", "ani_window",  "500");
    kerchunk_user_init(cfg);

    kerchevt_init();
    mock_reset();
    mock_init_core();

    kerchevt_subscribe(KERCHEVT_CALLER_IDENTIFIED, t_id_handler, NULL);
    kerchevt_subscribe(KERCHEVT_CALLER_CLEARED,    t_clear_handler, NULL);

    mod_caller.load(&g_mock_core);
    caller_configure(cfg);

    /* 1. ANI capture in window -> user 1 (ANI "5551") */
    test_begin("caller: ANI capture identifies user 1");
    reset_caller_track();
    mock_fire_simple(KERCHEVT_COR_ASSERT);   /* starts ANI window */
    mock_fire_dtmf('5');
    mock_fire_dtmf('5');
    mock_fire_dtmf('5');
    mock_fire_dtmf('1');
    test_assert(g_ani_timer >= 0, "no ANI timer");
    mock_fire_timer(g_ani_timer);           /* window closes, lookup */
    test_assert(t_caller_id == 1, "ANI not identified");
    test_assert(t_caller_method == KERCHUNK_CALLER_DTMF_ANI, "wrong method");
    test_end();

    /* 2. COR drop clears caller */
    test_begin("caller: COR drop clears caller");
    reset_caller_track();
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(t_caller_cleared == 1, "not cleared");
    test_assert(g_current_user_id == 0, "user ID not reset");
    test_end();

    /* 3. DTMF login *101# -> user 1 */
    test_begin("caller: DTMF login *101# identifies user 1");
    reset_caller_track();
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    if (g_ani_timer >= 0)
        mock_fire_timer(g_ani_timer);       /* close ANI window */
    mock_fire_dtmf('*');
    mock_fire_dtmf('1');
    mock_fire_dtmf('0');
    mock_fire_dtmf('1');
    mock_fire_dtmf('#');
    test_assert(t_caller_id == 1, "login not identified");
    test_assert(t_caller_method == KERCHUNK_CALLER_DTMF_LOGIN, "wrong method");
    test_assert(g_session_user_id == 1, "session not started");
    test_end();

    /* 4. Login session survives COR drop */
    test_begin("caller: login session survives COR drop");
    mock_fire_simple(KERCHEVT_COR_DROP);
    test_assert(g_session_user_id == 1, "session cleared on COR drop");
    test_assert(g_current_user_id == 0, "active caller not cleared");
    test_end();

    /* 5. Re-key restores identity from session */
    test_begin("caller: re-key restores session identity");
    reset_caller_track();
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    test_assert(t_caller_id == 1, "not re-identified from session");
    test_assert(t_caller_method == KERCHUNK_CALLER_DTMF_LOGIN, "wrong method");
    test_end();

    mock_fire_simple(KERCHEVT_COR_DROP);

    /* 6. Session timeout clears login */
    test_begin("caller: session timeout clears login");
    test_assert(g_session_timer >= 0, "no session timer");
    mock_fire_timer(g_session_timer);
    test_assert(g_session_user_id == 0, "session not cleared");
    /* Re-key should NOT re-identify */
    reset_caller_track();
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    if (g_ani_timer >= 0)
        mock_fire_timer(g_ani_timer);
    test_assert(t_caller_id == 0, "identified after session expired");
    test_end();

    mock_fire_simple(KERCHEVT_COR_DROP);

    mod_caller.unload();
    kerchevt_unsubscribe(KERCHEVT_CALLER_IDENTIFIED, t_id_handler);
    kerchevt_unsubscribe(KERCHEVT_CALLER_CLEARED,    t_clear_handler);
    kerchevt_shutdown();
    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);
}
