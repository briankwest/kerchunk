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
#include <string.h>

#define LOG_MOD "courtesy"

static kerchunk_core_t *g_core;

static int g_enabled = 1;          /* master enable (default on for compat) */

/* Default courtesy tone: 800 Hz, 100ms */
static int g_default_freq = 800;
static int g_default_dur  = 100;
static int16_t g_default_amp = 4000;
static int g_queue_courtesy = 0;  /* Also play after queue playback */

/* Current caller info (set by CALLER_IDENTIFIED) */
static int g_caller_id = 0;
static int g_courtesy_pending;  /* 1 while our own tone is in the queue */

static void queue_tone(const char *reason)
{
    if (!g_enabled) return;
    g_core->queue_silence(50, KERCHUNK_PRI_NORMAL);
    g_core->queue_tone(g_default_freq, g_default_dur, g_default_amp, KERCHUNK_PRI_NORMAL);
    g_courtesy_pending = 1;
    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "courtesy tone queued (%s)", reason);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "courtesy", .description = reason } };
    kerchevt_fire(&ae);
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    queue_tone(g_caller_id ? "cor_drop, caller" : "cor_drop");
}

static void on_queue_complete(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    /* Skip if this QUEUE_COMPLETE was from our own courtesy tone —
     * otherwise we chain endlessly: tone → complete → tone → ... */
    if (g_courtesy_pending) {
        g_courtesy_pending = 0;
        return;
    }
    if (g_queue_courtesy && !(kerchunk_queue_drain_flags() & QUEUE_FLAG_NO_TAIL))
        queue_tone("queue_complete");
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
    core->subscribe(KERCHEVT_QUEUE_COMPLETE, on_queue_complete, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    core->subscribe(KERCHEVT_CALLER_CLEARED, on_caller_cleared, NULL);
    return 0;
}

static int courtesy_configure(const kerchunk_config_t *cfg)
{
    const char *en = kerchunk_config_get(cfg, "courtesy", "enabled");
    g_enabled = (!en || strcmp(en, "off") != 0);  /* default on, "off" disables */

    g_default_freq = kerchunk_config_get_int(cfg, "courtesy", "freq", 800);
    g_default_dur  = kerchunk_config_get_duration_ms(cfg, "courtesy", "duration", 100);
    g_default_amp  = (int16_t)kerchunk_config_get_int(cfg, "courtesy", "amplitude", 4000);

    const char *qc = kerchunk_config_get(cfg, "courtesy", "queue_courtesy");
    g_queue_courtesy = (qc && strcmp(qc, "on") == 0);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "courtesy tone %s (%d Hz, %d ms)",
                g_enabled ? "enabled" : "disabled", g_default_freq, g_default_dur);
    return 0;
}

static void courtesy_unload(void)
{
    g_core->unsubscribe(KERCHEVT_COR_DROP, on_cor_drop);
    g_core->unsubscribe(KERCHEVT_QUEUE_COMPLETE, on_queue_complete);
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
