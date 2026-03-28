/*
 * mod_dtmfcmd.c — DTMF command trie + dispatch
 *
 * Accumulates digits after '*', walks trie, dispatches on '#'.
 * 3-second inter-digit timeout.
 * Fires custom events for other modules to handle.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>

#define LOG_MOD "dtmfcmd"
#define MAX_CMD_LEN 16
#define MAX_COMMANDS 64

/* Command table entry */
typedef struct {
    char    pattern[MAX_CMD_LEN];  /* e.g., "VS", "VM", "G1" */
    int     custom_event_id;       /* KERCHEVT_CUSTOM + N */
    char    description[64];
    char    config_key[32];        /* key for [dtmf] config override */
} dtmf_cmd_entry_t;

static kerchunk_core_t *g_core;

/* Command accumulation */
static int  g_active;
static char g_buf[MAX_CMD_LEN + 1];
static int  g_pos;
static int  g_timeout_timer = -1;
static int  g_timeout_ms = 3000;

/* COR gate: suppress DTMF during squelch transients */
static int  g_cor_gate_active;
static int  g_cor_gate_timer = -1;
static int  g_cor_gate_ms = 200;

/* Deferred command: wait for COR drop before dispatching */
static int  g_pending_dispatch;

/* Command table */
static dtmf_cmd_entry_t g_cmds[MAX_COMMANDS];
static int g_cmd_count;

/* Register a command pattern → custom event mapping.
 * Matches the dtmf_register vtable signature so modules can self-register. */
static int dtmf_register_cmd(const char *default_pattern, int event_offset,
                              const char *description, const char *config_key)
{
    if (g_cmd_count >= MAX_COMMANDS)
        return -1;
    dtmf_cmd_entry_t *c = &g_cmds[g_cmd_count++];
    snprintf(c->pattern, sizeof(c->pattern), "%s", default_pattern);
    c->custom_event_id = KERCHEVT_CUSTOM + event_offset;
    snprintf(c->description, sizeof(c->description), "%s", description);
    if (config_key)
        snprintf(c->config_key, sizeof(c->config_key), "%s", config_key);
    else
        c->config_key[0] = '\0';
    return 0;
}

/* Unregister a command by pattern */
static int dtmf_unregister_cmd(const char *pattern)
{
    for (int i = 0; i < g_cmd_count; i++) {
        if (strcmp(g_cmds[i].pattern, pattern) == 0) {
            /* Shift remaining entries down */
            for (int j = i; j < g_cmd_count - 1; j++)
                g_cmds[j] = g_cmds[j + 1];
            g_cmd_count--;
            return 0;
        }
    }
    return -1;
}

/* Built-in DTMF command event offsets */
#define DTMF_EVT_VOICEMAIL_STATUS  0   /* *VS# */
#define DTMF_EVT_VOICEMAIL_RECORD  1   /* *VM# */
#define DTMF_EVT_VOICEMAIL_PLAY    2   /* *VP# */
#define DTMF_EVT_VOICEMAIL_DELETE  3   /* *VD# */
#define DTMF_EVT_VOICEMAIL_LIST    4   /* *VL# */
#define DTMF_EVT_GPIO_ON           5   /* *G1<pin># */
#define DTMF_EVT_GPIO_OFF          6   /* *G0<pin># */
#define DTMF_EVT_ID                7   /* *ID<code># — handled by mod_caller */

static void cancel_timeout(void)
{
    if (g_timeout_timer >= 0) {
        g_core->timer_cancel(g_timeout_timer);
        g_timeout_timer = -1;
    }
}

static void reset_cmd(void)
{
    g_active = 0;
    g_pos = 0;
    g_buf[0] = '\0';
    cancel_timeout();
}

static void timeout_cb(void *ud)
{
    (void)ud;
    g_timeout_timer = -1;
    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "DTMF command timeout, buf='%s'", g_buf);
    reset_cmd();
}

static void restart_timeout(void)
{
    cancel_timeout();
    g_timeout_timer = g_core->timer_create(g_timeout_ms, 0, timeout_cb, NULL);
}

/* Try to match accumulated buffer against command table */
static int dispatch_command(void)
{
    g_buf[g_pos] = '\0';

    for (int i = 0; i < g_cmd_count; i++) {
        const char *pat = g_cmds[i].pattern;
        size_t plen = strlen(pat);

        /* Pattern is a prefix match — remaining chars are the argument */
        if (g_pos >= (int)plen && strncmp(g_buf, pat, plen) == 0) {
            /* Fire custom event with the argument portion.
             * arg is stack-allocated but safe: fire_event is synchronous,
             * so all subscribers complete before this function returns.
             * Subscribers must NOT store evt->custom.data pointers. */
            char arg[MAX_CMD_LEN];
            snprintf(arg, sizeof(arg), "%s", g_buf + plen);

            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "command: *%s# → event %d, arg='%s'",
                        g_buf, g_cmds[i].custom_event_id, arg);

            kerchevt_t evt = {
                .type = (kerchevt_type_t)g_cmds[i].custom_event_id,
                .custom = { .data = arg, .len = strlen(arg) + 1 },
            };
            g_core->fire_event(&evt);
            return 0;
        }
    }

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "unknown command: *%s#", g_buf);
    return -1;
}

static void cor_gate_expired(void *ud)
{
    (void)ud;
    g_cor_gate_timer = -1;
    g_cor_gate_active = 0;
}

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_cor_gate_ms <= 0) return;
    g_cor_gate_active = 1;
    if (g_cor_gate_timer >= 0)
        g_core->timer_cancel(g_cor_gate_timer);
    g_cor_gate_timer = g_core->timer_create(g_cor_gate_ms, 0, cor_gate_expired, NULL);
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    /* COR gate on drop too (some radios send spurious DTMF on squelch close) */
    if (g_cor_gate_ms > 0) {
        g_cor_gate_active = 1;
        if (g_cor_gate_timer >= 0)
            g_core->timer_cancel(g_cor_gate_timer);
        g_cor_gate_timer = g_core->timer_create(g_cor_gate_ms, 0, cor_gate_expired, NULL);
    }

    /* Dispatch deferred command now that user unkeyed */
    if (g_pending_dispatch) {
        g_pending_dispatch = 0;
        dispatch_command();
        reset_cmd();
    }
}

static void on_dtmf_digit(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    char d = evt->dtmf.digit;

    /* Suppress digits during COR gate (squelch transient protection) */
    if (g_cor_gate_active && d != '*' && d != '#')
        return;

    if (d == '*') {
        /* Start new command */
        g_active = 1;
        g_pos = 0;
        g_buf[0] = '\0';
        restart_timeout();
        return;
    }

    if (!g_active)
        return;

    if (d == '#') {
        /* End of command — defer dispatch until COR drops so the
         * announcement doesn't play while the user is still keyed up. */
        cancel_timeout();
        if (g_core->is_receiving()) {
            g_pending_dispatch = 1;
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "command *%s# ready, waiting for COR drop", g_buf);
        } else {
            dispatch_command();
            reset_cmd();
        }
        return;
    }

    /* Accumulate digit */
    if (g_pos < MAX_CMD_LEN) {
        /* Map DTMF digits to command characters:
         * 2=ABC, 3=DEF, 4=GHI, 5=JKL, 6=MNO, 7=PQRS, 8=TUV, 9=WXYZ
         * For simplicity, map to first letter of group.
         * But we also allow raw digits for numeric args. */

        /* Store raw digit — patterns use digit chars */
        g_buf[g_pos++] = d;
        restart_timeout();
    } else {
        /* Command too long */
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "command too long, resetting");
        reset_cmd();
    }
}

static int dtmfcmd_load(kerchunk_core_t *core)
{
    g_core = core;
    g_cmd_count = 0;
    reset_cmd();

    /* Expose registration API via core vtable so consumer modules
     * can register their own DTMF commands during load(). */
    core->dtmf_register   = dtmf_register_cmd;
    core->dtmf_unregister = dtmf_unregister_cmd;

    core->subscribe(KERCHEVT_DTMF_DIGIT, on_dtmf_digit, NULL);
    core->subscribe(KERCHEVT_COR_ASSERT, on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP, on_cor_drop, NULL);
    return 0;
}

static int dtmfcmd_configure(const kerchunk_config_t *cfg)
{
    g_timeout_ms = kerchunk_config_get_int(cfg, "dtmf", "inter_digit_timeout", 3000);
    g_cor_gate_ms = kerchunk_config_get_int(cfg, "dtmf", "cor_gate_ms", 200);

    /* Check for pattern overrides from [dtmf] config section */
    for (int i = 0; i < g_cmd_count; i++) {
        if (g_cmds[i].config_key[0] == '\0')
            continue;
        const char *override = kerchunk_config_get(cfg, "dtmf", g_cmds[i].config_key);
        if (override && override[0] != '\0') {
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "override: %s pattern '%s' -> '%s'",
                        g_cmds[i].config_key, g_cmds[i].pattern, override);
            snprintf(g_cmds[i].pattern, sizeof(g_cmds[i].pattern), "%s", override);
        }
    }

    return 0;
}

static void dtmfcmd_unload(void)
{
    g_core->unsubscribe(KERCHEVT_DTMF_DIGIT, on_dtmf_digit);
    g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP, on_cor_drop);
    cancel_timeout();
    if (g_cor_gate_timer >= 0) {
        g_core->timer_cancel(g_cor_gate_timer);
        g_cor_gate_timer = -1;
    }
    g_core->dtmf_register   = NULL;
    g_core->dtmf_unregister = NULL;
}

/* CLI */
static int cli_dtmfcmd(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    /* JSON: array of command objects */
    if (!r->jfirst) resp_json_raw(r, ",");
    resp_json_raw(r, "\"commands\":[");
    for (int i = 0; i < g_cmd_count; i++) {
        if (i > 0) resp_json_raw(r, ",");
        char frag[8192];
        snprintf(frag, sizeof(frag),
                 "{\"pattern\":\"*%s#\",\"description\":\"%s\"}",
                 g_cmds[i].pattern, g_cmds[i].description);
        resp_json_raw(r, frag);
    }
    resp_json_raw(r, "]");
    r->jfirst = 0;
    resp_bool(r, "active", g_active);
    resp_str(r, "buffer", g_buf);
    /* Text */
    resp_text_raw(r, "DTMF Commands:\n");
    for (int i = 0; i < g_cmd_count; i++) {
        char line[8192];
        snprintf(line, sizeof(line), "  *%s# -> %s\n",
                 g_cmds[i].pattern, g_cmds[i].description);
        resp_text_raw(r, line);
    }
    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "dtmfcmd", "dtmfcmd", "Show DTMF command table", cli_dtmfcmd },
};

static kerchunk_module_def_t mod_dtmfcmd = {
    .name         = "mod_dtmfcmd",
    .version      = "1.0.0",
    .description  = "DTMF command router",
    .load         = dtmfcmd_load,
    .configure    = dtmfcmd_configure,
    .unload       = dtmfcmd_unload,
    .cli_commands = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_dtmfcmd);
