/*
 * mod_courtesy.c — Courtesy tones
 *
 * Plays a short tone when COR drops (user unkeys).
 * Can be caller-specific if caller is identified.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>

#define LOG_MOD "courtesy"

static kerchunk_core_t *g_core;

/* Default courtesy tone: 800 Hz, 100ms */
static int g_default_freq = 800;
static int g_default_dur  = 100;
static int16_t g_default_amp = 4000;

/* Current caller info (set by CALLER_IDENTIFIED) */
static int g_caller_id = 0;

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;

    /* Queue courtesy tone */
    g_core->queue_silence(50, 2);  /* Brief pause */
    g_core->queue_tone(g_default_freq, g_default_dur, g_default_amp, 2);

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "courtesy tone queued (caller=%d)", g_caller_id);
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_caller_id = evt->caller.user_id;
}

static void on_caller_cleared(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_caller_id = 0;
}

static int courtesy_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_COR_DROP, on_cor_drop, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    core->subscribe(KERCHEVT_CALLER_CLEARED, on_caller_cleared, NULL);
    return 0;
}

static int courtesy_configure(const kerchunk_config_t *cfg)
{
    g_default_freq = kerchunk_config_get_int(cfg, "courtesy", "freq", 800);
    g_default_dur  = kerchunk_config_get_int(cfg, "courtesy", "duration", 100);
    g_default_amp  = (int16_t)kerchunk_config_get_int(cfg, "courtesy", "amplitude", 4000);
    return 0;
}

static void courtesy_unload(void)
{
    g_core->unsubscribe(KERCHEVT_COR_DROP, on_cor_drop);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_CALLER_CLEARED, on_caller_cleared);
}

static kerchunk_module_def_t mod_courtesy = {
    .name        = "mod_courtesy",
    .version     = "1.0.0",
    .description = "Courtesy tones",
    .load        = courtesy_load,
    .configure   = courtesy_configure,
    .unload      = courtesy_unload,
};

KERCHUNK_MODULE_DEFINE(mod_courtesy);
