/*
 * test_integ_txcode.c — Integration tests for mod_txcode
 *
 * Tests: caller sets encoder, cleared destroys, group fallback.
 */

#include "test_integ_mock.h"

/* Pull in the module source */
#include "../modules/mod_txcode.c"

void test_integ_txcode(void)
{
    /* Set up user database with groups */
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "group.1", "name",     "Family");
    kerchunk_config_set(cfg, "group.1", "tx_ctcss", "1000");
    kerchunk_config_set(cfg, "user.1",  "name",     "Brian");
    kerchunk_config_set(cfg, "user.1",  "ctcss",    "1000");
    kerchunk_config_set(cfg, "user.1",  "group",    "1");
    kerchunk_config_set(cfg, "user.1",  "tx_ctcss", "1000");
    kerchunk_config_set(cfg, "user.2",  "name",     "Alice");
    kerchunk_config_set(cfg, "user.2",  "ctcss",    "1318");
    kerchunk_config_set(cfg, "user.2",  "group",    "1");
    /* Alice has no tx_ctcss — falls back to group */
    kerchunk_config_set(cfg, "user.3",  "name",     "Charlie");
    kerchunk_config_set(cfg, "user.3",  "ctcss",    "770");
    /* Charlie: no group, no tx_ctcss */
    kerchunk_user_init(cfg);

    kerchevt_init();
    mock_reset();
    mock_init_core();

    mod_txcode.load(&g_mock_core);
    /* Set no repeater default */
    kerchunk_config_set(cfg, "repeater", "tx_ctcss", "0");
    txcode_configure(cfg);

    /* 1. Caller identified with user tx_ctcss → encoder created */
    test_begin("txcode: caller sets CTCSS encoder");
    {
        kerchevt_t e = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = 1, .method = KERCHUNK_CALLER_DTMF_ANI },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_enc != NULL, "encoder not created");
    test_assert(g_enc_type == KERCHUNK_TX_ENC_CTCSS, "wrong encoder type");
    test_end();

    /* 2. Caller cleared → encoder destroyed */
    test_begin("txcode: caller cleared destroys encoder");
    mock_fire_simple(KERCHEVT_CALLER_CLEARED);
    test_assert(g_enc == NULL, "encoder not destroyed");
    test_assert(g_enc_type == KERCHUNK_TX_ENC_NONE, "type not cleared");
    test_end();

    /* 3. Group fallback: Alice has no tx_ctcss but group does */
    test_begin("txcode: group fallback for TX tone");
    {
        kerchevt_t e = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = 2, .method = KERCHUNK_CALLER_DTMF_ANI },
        };
        kerchevt_fire(&e);
    }
    test_assert(g_enc != NULL, "encoder not created from group");
    test_assert(g_enc_type == KERCHUNK_TX_ENC_CTCSS, "wrong type from group");
    test_end();

    /* Clean up encoder before unload */
    mock_fire_simple(KERCHEVT_CALLER_CLEARED);

    mod_txcode.unload();
    kerchevt_shutdown();
    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);
}
