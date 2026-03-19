/*
 * mod_repeater.c — Repeater state machine
 *
 * States: IDLE → RECEIVING → TAIL_WAIT → HANG_WAIT → IDLE, plus TIMEOUT
 * Timers: debounce, tail, hang, TOT
 *
 * When require_identification is on (closed repeater), COR debounce is
 * extended to at least 500ms to allow CTCSS/DCS detection. If no caller
 * is identified by debounce expiry, access is denied. DTMF login users
 * must login first (*code#), then rekey.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>

#define LOG_MOD "repeater"

/* Repeater states */
enum {
    RPT_IDLE,
    RPT_RECEIVING,
    RPT_TAIL_WAIT,
    RPT_HANG_WAIT,
    RPT_TIMEOUT,
};

static const char *state_name(int s)
{
    switch (s) {
    case RPT_IDLE:      return "IDLE";
    case RPT_RECEIVING: return "RECEIVING";
    case RPT_TAIL_WAIT: return "TAIL_WAIT";
    case RPT_HANG_WAIT: return "HANG_WAIT";
    case RPT_TIMEOUT:   return "TIMEOUT";
    default:            return "???";
    }
}

static kerchunk_core_t *g_core;
static int g_state = RPT_IDLE;

/* Timer IDs */
static int g_tail_timer     = -1;
static int g_hang_timer     = -1;
static int g_tot_timer      = -1;
static int g_debounce_timer = -1;

/* Config */
static int g_tail_time_ms       = 2000;
static int g_hang_time_ms       = 500;
static int g_tot_time_ms        = 180000;
static int g_tot_warn_hz        = 440;
static int g_cor_debounce_ms    = 150;
static int g_require_id         = 0;

/* Caller tracking (for closed repeater) */
static int g_caller_identified  = 0;

static void change_state(int new_state)
{
    if (new_state == g_state)
        return;

    int old = g_state;
    g_state = new_state;

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "state: %s -> %s",
                state_name(old), state_name(new_state));

    kerchevt_t evt = {
        .type = KERCHEVT_STATE_CHANGE,
        .state = { .old_state = old, .new_state = new_state },
    };
    g_core->fire_event(&evt);
}

/* ── Timer callbacks ── */

static void tail_expired(void *ud)
{
    (void)ud;
    g_tail_timer = -1;

    if (g_state == RPT_TAIL_WAIT) {
        kerchevt_t evt = { .type = KERCHEVT_TAIL_EXPIRE };
        g_core->fire_event(&evt);
        change_state(RPT_HANG_WAIT);
        g_hang_timer = g_core->timer_create(g_hang_time_ms, 0,
                                             tail_expired, NULL);
    } else if (g_state == RPT_HANG_WAIT) {
        g_hang_timer = -1;
        g_core->release_ptt("repeater");
        change_state(RPT_IDLE);
    }
}

static void tot_expired(void *ud);

static void start_receiving(void)
{
    g_core->request_ptt("repeater");
    change_state(RPT_RECEIVING);

    if (!kerchunk_core_get_emergency())
        g_tot_timer = g_core->timer_create(g_tot_time_ms, 0,
                                            tot_expired, NULL);
}

static void debounce_expired(void *ud)
{
    (void)ud;
    g_debounce_timer = -1;

    if (g_state != RPT_IDLE)
        return;

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "COR debounce passed");

    if (g_require_id && !g_caller_identified) {
        /*
         * Closed repeater: caller was not identified during the debounce
         * window. Decoders have been running the whole time — if CTCSS/DCS
         * wasn't detected by now, they don't have the right tone.
         *
         * DTMF login users: login first (*code#), then rekey. The login
         * session persists (mod_caller) and sets g_caller_identified on
         * the next COR via CALLER_IDENTIFIED event.
         */
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "access denied — no identification");

        if (g_core->tts_speak)
            g_core->tts_speak("Access denied.", 4);
        else
            g_core->queue_tone(400, 500, 4000, 4);

        return;  /* Stay in IDLE */
    }

    start_receiving();
}

static void tot_expired(void *ud)
{
    (void)ud;
    g_tot_timer = -1;

    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "TOT fired!");
    g_core->queue_tone(g_tot_warn_hz, 1000, 8000, 10);

    kerchevt_t evt = { .type = KERCHEVT_TIMEOUT };
    g_core->fire_event(&evt);

    change_state(RPT_TIMEOUT);
}

/* ── Event handlers ── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;

    switch (g_state) {
    case RPT_IDLE:
        g_caller_identified = 0;  /* Reset for new COR cycle */

        if (g_cor_debounce_ms > 0 || g_require_id) {
            /*
             * Debounce COR. When require_identification is on, use at
             * least 500ms to give CTCSS/DCS decoders time to identify.
             * The decoders run on captured audio regardless of PTT state.
             */
            int debounce = g_cor_debounce_ms;
            if (g_require_id && debounce < 500)
                debounce = 500;

            if (g_debounce_timer >= 0)
                g_core->timer_cancel(g_debounce_timer);
            g_debounce_timer = g_core->timer_create(
                debounce, 0, debounce_expired, NULL);
        } else {
            /* No debounce, open access — immediate transition */
            start_receiving();
        }
        break;

    case RPT_TAIL_WAIT:
        /* Rekey — cancel tail timer, back to receiving (already ID'd) */
        if (g_tail_timer >= 0) {
            g_core->timer_cancel(g_tail_timer);
            g_tail_timer = -1;
        }
        change_state(RPT_RECEIVING);
        break;

    case RPT_HANG_WAIT:
        /* Rekey from hang — already identified in this session */
        if (g_hang_timer >= 0) {
            g_core->timer_cancel(g_hang_timer);
            g_hang_timer = -1;
        }
        change_state(RPT_RECEIVING);
        /* Restart TOT */
        if (g_tot_timer >= 0)
            g_core->timer_cancel(g_tot_timer);
        g_tot_timer = g_core->timer_create(g_tot_time_ms, 0,
                                            tot_expired, NULL);
        break;

    case RPT_TIMEOUT:
        /* Ignore — user must unkey first */
        break;

    default:
        break;
    }
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;

    /* Cancel debounce if COR drops before it expires (kerchunk filtered) */
    if (g_debounce_timer >= 0) {
        g_core->timer_cancel(g_debounce_timer);
        g_debounce_timer = -1;
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "kerchunk filtered");
        return;
    }

    switch (g_state) {
    case RPT_RECEIVING:
        if (g_tot_timer >= 0) {
            g_core->timer_cancel(g_tot_timer);
            g_tot_timer = -1;
        }
        change_state(RPT_TAIL_WAIT);
        g_tail_timer = g_core->timer_create(g_tail_time_ms, 0,
                                             tail_expired, NULL);

        kerchevt_t tail_evt = { .type = KERCHEVT_TAIL_START };
        g_core->fire_event(&tail_evt);
        break;

    case RPT_TIMEOUT:
        g_core->release_ptt("repeater");
        change_state(RPT_IDLE);
        break;

    default:
        break;
    }
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_caller_identified = 1;

    /*
     * If we're in IDLE with debounce running (closed repeater waiting for
     * ID), the debounce callback will check g_caller_identified when it
     * fires and allow entry to RECEIVING.
     */
}

static void on_caller_cleared(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    /* Don't clear during an active session — only on new COR cycle */
    if (g_state == RPT_IDLE)
        g_caller_identified = 0;
}

/* ── Module lifecycle ── */

static int repeater_load(kerchunk_core_t *core)
{
    g_core = core;
    g_state = RPT_IDLE;
    core->subscribe(KERCHEVT_COR_ASSERT,       on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,          on_cor_drop, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    core->subscribe(KERCHEVT_CALLER_CLEARED,    on_caller_cleared, NULL);
    return 0;
}

static int repeater_configure(const kerchunk_config_t *cfg)
{
    g_tail_time_ms    = kerchunk_config_get_int(cfg, "repeater", "tail_time", 2000);
    g_hang_time_ms    = kerchunk_config_get_int(cfg, "repeater", "hang_time", 500);
    g_tot_time_ms     = kerchunk_config_get_int(cfg, "repeater", "timeout_time", 180000);
    g_cor_debounce_ms = kerchunk_config_get_int(cfg, "repeater", "cor_debounce", 150);

    /* Validate ranges */
    if (g_tail_time_ms < 0)    g_tail_time_ms = 0;
    if (g_tail_time_ms > 30000) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "tail_time capped at 30s");
        g_tail_time_ms = 30000;
    }
    if (g_hang_time_ms < 0)     g_hang_time_ms = 0;
    if (g_hang_time_ms > 10000) g_hang_time_ms = 10000;
    if (g_tot_time_ms < 10000) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "timeout_time minimum 10s");
        g_tot_time_ms = 10000;
    }
    if (g_tot_time_ms > 600000) g_tot_time_ms = 600000;
    if (g_cor_debounce_ms < 0)  g_cor_debounce_ms = 0;
    if (g_cor_debounce_ms > 5000) g_cor_debounce_ms = 5000;

    const char *ri = kerchunk_config_get(cfg, "repeater", "require_identification");
    g_require_id = (ri && strcmp(ri, "on") == 0);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "tail=%dms hang=%dms tot=%dms debounce=%dms require_id=%s",
                g_tail_time_ms, g_hang_time_ms, g_tot_time_ms,
                g_cor_debounce_ms, g_require_id ? "on" : "off");
    return 0;
}

static void repeater_unload(void)
{
    g_core->unsubscribe(KERCHEVT_COR_ASSERT,       on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,          on_cor_drop);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_CALLER_CLEARED,    on_caller_cleared);

    if (g_tail_timer >= 0)      g_core->timer_cancel(g_tail_timer);
    if (g_hang_timer >= 0)      g_core->timer_cancel(g_hang_timer);
    if (g_tot_timer >= 0)       g_core->timer_cancel(g_tot_timer);
    if (g_debounce_timer >= 0)  g_core->timer_cancel(g_debounce_timer);
}

/* CLI */
static int cli_repeater(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    resp_str(r, "state", state_name(g_state));
    resp_str(r, "access", g_require_id ? "closed" : "open");
    resp_bool(r, "caller_identified", g_caller_identified);
    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "repeater", "repeater", "Show repeater state", cli_repeater },
};

static kerchunk_module_def_t mod_repeater = {
    .name         = "mod_repeater",
    .version      = "1.0.0",
    .description  = "Repeater state machine",
    .load         = repeater_load,
    .configure    = repeater_configure,
    .unload       = repeater_unload,
    .cli_commands = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_repeater);
