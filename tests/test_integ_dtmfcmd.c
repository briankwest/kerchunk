/*
 * test_integ_dtmfcmd.c — Integration tests for DTMF command dispatch
 *
 * Includes mod_dtmfcmd.c directly.
 * Tests digit accumulation, command dispatch, args, timeout, overflow.
 */

#include "test_integ_mock.h"

/* Pull in the module source */
#include "../modules/mod_dtmfcmd.c"

/* ---- custom event tracking ---- */

static int           t_custom_fired;
static kerchevt_type_t t_custom_type;
static char          t_custom_arg[32];

static void custom_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    t_custom_fired++;
    t_custom_type = evt->type;
    if (evt->custom.data && evt->custom.len > 0) {
        size_t n = evt->custom.len;
        if (n > sizeof(t_custom_arg) - 1)
            n = sizeof(t_custom_arg) - 1;
        memcpy(t_custom_arg, evt->custom.data, n);
        t_custom_arg[n] = '\0';
    } else {
        t_custom_arg[0] = '\0';
    }
}

static void reset_custom(void)
{
    t_custom_fired = 0;
    t_custom_type  = 0;
    t_custom_arg[0] = '\0';
}

/* ---- entry point ---- */

void test_integ_dtmfcmd(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    /* Subscribe test handler to all registered custom event offsets */
    for (int i = 0; i <= 8; i++)
        kerchevt_subscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i), custom_handler, NULL);

    mod_dtmfcmd.load(&g_mock_core);

    /* Register commands that consumer modules would normally register */
    dtmf_register_cmd("87", DTMF_EVT_VOICEMAIL_STATUS, "Voicemail status", "voicemail_status");
    dtmf_register_cmd("86", DTMF_EVT_VOICEMAIL_RECORD, "Voicemail record", "voicemail_record");
    dtmf_register_cmd("85", DTMF_EVT_VOICEMAIL_PLAY,   "Voicemail play",   "voicemail_play");
    dtmf_register_cmd("83", DTMF_EVT_VOICEMAIL_DELETE,  "Voicemail delete", "voicemail_delete");
    dtmf_register_cmd("84", DTMF_EVT_VOICEMAIL_LIST,    "Voicemail list",   "voicemail_list");
    dtmf_register_cmd("41", DTMF_EVT_GPIO_ON,           "GPIO on",          "gpio_on");
    dtmf_register_cmd("40", DTMF_EVT_GPIO_OFF,          "GPIO off",         "gpio_off");
    dtmf_register_cmd("93", 8,                           "Weather report",   "weather_report");

    /* 1. '*' begins accumulation */
    test_begin("dtmfcmd: * begins accumulation");
    mock_fire_dtmf('*');
    test_assert(g_active == 1, "not active");
    test_assert(g_pos == 0, "pos not 0");
    test_end();

    /* 2. digit accumulation */
    test_begin("dtmfcmd: digit accumulation");
    mock_fire_dtmf('8');
    mock_fire_dtmf('7');
    test_assert(g_pos == 2, "pos wrong");
    test_assert(g_buf[0] == '8' && g_buf[1] == '7', "buf wrong");
    test_end();

    /* 3. *87# dispatches voicemail status (KERCHEVT_CUSTOM+0) */
    test_begin("dtmfcmd: *87# dispatches VM status");
    reset_custom();
    mock_fire_dtmf('#');
    test_assert(t_custom_fired >= 1, "event not fired");
    test_assert(t_custom_type == (kerchevt_type_t)(KERCHEVT_CUSTOM + DTMF_EVT_VOICEMAIL_STATUS),
                "wrong event type");
    test_assert(t_custom_arg[0] == '\0', "unexpected arg");
    test_end();

    /* 4. *413# dispatches GPIO on (KERCHEVT_CUSTOM+5) with arg "3" */
    test_begin("dtmfcmd: *413# dispatches GPIO on arg=3");
    reset_custom();
    mock_fire_dtmf('*');
    mock_fire_dtmf('4');
    mock_fire_dtmf('1');
    mock_fire_dtmf('3');
    mock_fire_dtmf('#');
    test_assert(t_custom_fired >= 1, "event not fired");
    test_assert(t_custom_type == (kerchevt_type_t)(KERCHEVT_CUSTOM + DTMF_EVT_GPIO_ON),
                "wrong event type");
    test_assert(strcmp(t_custom_arg, "3") == 0, "wrong arg");
    test_end();

    /* 5. *999# unknown command — no custom event */
    test_begin("dtmfcmd: *999# unknown command");
    reset_custom();
    mock_fire_dtmf('*');
    mock_fire_dtmf('9');
    mock_fire_dtmf('9');
    mock_fire_dtmf('9');
    mock_fire_dtmf('#');
    test_assert(t_custom_fired == 0, "spurious event");
    test_end();

    /* 6. inter-digit timeout resets state */
    test_begin("dtmfcmd: timeout resets accumulation");
    mock_fire_dtmf('*');
    mock_fire_dtmf('8');
    test_assert(g_active == 1, "not active");
    test_assert(g_timeout_timer >= 0, "no timeout timer");
    mock_fire_timer(g_timeout_timer);
    test_assert(g_active == 0, "still active after timeout");
    test_assert(g_pos == 0, "pos not reset");
    test_end();

    /* 7. overflow resets state */
    test_begin("dtmfcmd: overflow resets accumulation");
    mock_fire_dtmf('*');
    for (int i = 0; i < MAX_CMD_LEN + 1; i++)
        mock_fire_dtmf('5');
    test_assert(g_active == 0, "still active after overflow");
    test_end();

    /* 8. sequential commands both dispatch */
    test_begin("dtmfcmd: sequential *86# dispatches VM record");
    reset_custom();
    mock_fire_dtmf('*');
    mock_fire_dtmf('8');
    mock_fire_dtmf('6');
    mock_fire_dtmf('#');
    test_assert(t_custom_fired >= 1, "event not fired");
    test_assert(t_custom_type == (kerchevt_type_t)(KERCHEVT_CUSTOM + DTMF_EVT_VOICEMAIL_RECORD),
                "wrong event type");
    test_end();

    /* 9. COR gate suppresses digits during gate period */
    test_begin("dtmfcmd: COR gate suppresses digits");
    {
        kerchunk_config_t *cfg = kerchunk_config_create();
        kerchunk_config_set(cfg, "dtmf", "cor_gate_ms", "200");
        dtmfcmd_configure(cfg);
        kerchunk_config_destroy(cfg);

        reset_custom();
        mock_fire_simple(KERCHEVT_COR_ASSERT);
        /* Digit during gate should be suppressed */
        mock_fire_dtmf('*');
        mock_fire_dtmf('8');
        mock_fire_dtmf('7');
        mock_fire_dtmf('#');
        test_assert(t_custom_fired == 0, "digit during gate should be suppressed");
    }
    test_end();

    /* 10. COR gate expires, digits accepted */
    test_begin("dtmfcmd: COR gate expires, digits accepted");
    {
        /* Fire the gate timer to simulate expiry */
        test_assert(g_cor_gate_timer >= 0, "gate timer not set");
        mock_fire_timer(g_cor_gate_timer);
        test_assert(g_cor_gate_active == 0, "gate still active after expiry");

        reset_custom();
        mock_fire_dtmf('*');
        mock_fire_dtmf('8');
        mock_fire_dtmf('7');
        mock_fire_dtmf('#');
        test_assert(t_custom_fired >= 1, "digit after gate should dispatch");
        test_assert(t_custom_type == (kerchevt_type_t)(KERCHEVT_CUSTOM + DTMF_EVT_VOICEMAIL_STATUS),
                    "wrong event after gate");
    }
    test_end();

    /* 11. COR gate disabled (cor_gate_ms=0) passes all digits */
    test_begin("dtmfcmd: COR gate disabled passes digits");
    {
        kerchunk_config_t *cfg = kerchunk_config_create();
        kerchunk_config_set(cfg, "dtmf", "cor_gate_ms", "0");
        dtmfcmd_configure(cfg);
        kerchunk_config_destroy(cfg);

        reset_custom();
        mock_fire_simple(KERCHEVT_COR_ASSERT);
        mock_fire_dtmf('*');
        mock_fire_dtmf('8');
        mock_fire_dtmf('7');
        mock_fire_dtmf('#');
        test_assert(t_custom_fired >= 1, "digits should pass when gate disabled");
    }
    test_end();

    mod_dtmfcmd.unload();
    for (int i = 0; i <= 8; i++)
        kerchevt_unsubscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i), custom_handler);
    kerchevt_shutdown();
}
