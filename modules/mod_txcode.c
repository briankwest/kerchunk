/*
 * mod_txcode.c — Dynamic TX CTCSS/DCS encoder
 *
 * Sets the active TX encoder based on the identified caller's
 * TX tone configuration (user → group fallback → repeater default).
 * The audio thread mixes the encoder output into outbound audio.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "plcode.h"
#include <stdio.h>
#include <stdlib.h>

#define LOG_MOD "txcode"
#define RATE    8000

static kerchunk_core_t *g_core;

/* Config: repeater-wide default TX tone */
static uint16_t g_default_tx_ctcss;
static uint16_t g_default_tx_dcs;
static int16_t  g_ctcss_amplitude = 800;  /* CTCSS tone amplitude (configurable) */

/* Current encoder */
static void *g_enc;
static int   g_enc_type;

static void destroy_encoder(void)
{
    if (!g_enc) return;

    if (g_enc_type == KERCHUNK_TX_ENC_CTCSS)
        plcode_ctcss_enc_destroy(g_enc);
    else if (g_enc_type == KERCHUNK_TX_ENC_DCS)
        plcode_dcs_enc_destroy(g_enc);

    g_enc      = NULL;
    g_enc_type = KERCHUNK_TX_ENC_NONE;
    kerchunk_core_set_tx_encoder(NULL, KERCHUNK_TX_ENC_NONE);
}

static void create_encoder(uint16_t ctcss_freq_x10, uint16_t dcs_code)
{
    destroy_encoder();

    if (ctcss_freq_x10 > 0) {
        plcode_ctcss_enc_t *enc = NULL;
        if (plcode_ctcss_enc_create(&enc, RATE, ctcss_freq_x10, g_ctcss_amplitude) == PLCODE_OK) {
            g_enc      = enc;
            g_enc_type = KERCHUNK_TX_ENC_CTCSS;
            kerchunk_core_set_tx_encoder(enc, KERCHUNK_TX_ENC_CTCSS);
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "TX CTCSS encoder: %.1f Hz",
                        (float)ctcss_freq_x10 / 10.0f);
        }
    } else if (dcs_code > 0) {
        plcode_dcs_enc_t *enc = NULL;
        if (plcode_dcs_enc_create(&enc, RATE, dcs_code, 0, 1600) == PLCODE_OK) {
            g_enc      = enc;
            g_enc_type = KERCHUNK_TX_ENC_DCS;
            kerchunk_core_set_tx_encoder(enc, KERCHUNK_TX_ENC_DCS);
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "TX DCS encoder: %u", dcs_code);
        }
    }
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    int user_id = evt->caller.user_id;

    uint16_t ctcss = 0, dcs = 0;
    if (kerchunk_user_lookup_group_tx(user_id, &ctcss, &dcs) == 0 &&
        (ctcss > 0 || dcs > 0)) {
        create_encoder(ctcss, dcs);
    } else if (g_default_tx_ctcss > 0 || g_default_tx_dcs > 0) {
        create_encoder(g_default_tx_ctcss, g_default_tx_dcs);
    } else {
        destroy_encoder();
    }
}

static void on_caller_cleared(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;

    /* Revert to repeater default or none */
    if (g_default_tx_ctcss > 0 || g_default_tx_dcs > 0)
        create_encoder(g_default_tx_ctcss, g_default_tx_dcs);
    else
        destroy_encoder();
}

static int txcode_load(kerchunk_core_t *core)
{
    g_core = core;
    g_enc      = NULL;
    g_enc_type = KERCHUNK_TX_ENC_NONE;
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    core->subscribe(KERCHEVT_CALLER_CLEARED,    on_caller_cleared, NULL);
    return 0;
}

static int txcode_configure(const kerchunk_config_t *cfg)
{
    g_default_tx_ctcss = (uint16_t)kerchunk_config_get_int(cfg, "repeater", "tx_ctcss", 0);
    g_default_tx_dcs   = (uint16_t)kerchunk_config_get_int(cfg, "repeater", "tx_dcs", 0);
    g_ctcss_amplitude  = (int16_t)kerchunk_config_get_int(cfg, "repeater", "ctcss_amplitude", 800);
    if (g_ctcss_amplitude < 100)   g_ctcss_amplitude = 100;
    if (g_ctcss_amplitude > 4000)  g_ctcss_amplitude = 4000;

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "default tx_ctcss=%u tx_dcs=%u ctcss_amplitude=%d",
                g_default_tx_ctcss, g_default_tx_dcs, g_ctcss_amplitude);
    return 0;
}

static void txcode_unload(void)
{
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_CALLER_CLEARED,    on_caller_cleared);
    destroy_encoder();
}

/* CLI */
static int cli_txcode(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    const char *type_str = "none";
    if (g_enc_type == KERCHUNK_TX_ENC_CTCSS) type_str = "CTCSS";
    else if (g_enc_type == KERCHUNK_TX_ENC_DCS) type_str = "DCS";
    resp_str(r, "tx_encoder", type_str);
    resp_int(r, "default_ctcss", g_default_tx_ctcss);
    resp_int(r, "default_dcs", g_default_tx_dcs);
    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "txcode", "txcode", "TX encoder status", cli_txcode },
};

static kerchunk_module_def_t mod_txcode = {
    .name             = "mod_txcode",
    .version          = "1.0.0",
    .description      = "Dynamic TX CTCSS/DCS encoder",
    .load             = txcode_load,
    .configure        = txcode_configure,
    .unload           = txcode_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_txcode);
