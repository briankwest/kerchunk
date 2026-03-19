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

/* Command table */
static dtmf_cmd_entry_t g_cmds[MAX_COMMANDS];
static int g_cmd_count;

/* Register a command pattern → custom event mapping */
static void register_cmd(const char *pattern, int event_offset, const char *desc)
{
    if (g_cmd_count >= MAX_COMMANDS)
        return;
    dtmf_cmd_entry_t *c = &g_cmds[g_cmd_count++];
    snprintf(c->pattern, sizeof(c->pattern), "%s", pattern);
    c->custom_event_id = KERCHEVT_CUSTOM + event_offset;
    snprintf(c->description, sizeof(c->description), "%s", desc);
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

static void on_cor_event(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_cor_gate_ms <= 0) return;
    g_cor_gate_active = 1;
    if (g_cor_gate_timer >= 0)
        g_core->timer_cancel(g_cor_gate_timer);
    g_cor_gate_timer = g_core->timer_create(g_cor_gate_ms, 0, cor_gate_expired, NULL);
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
        /* End of command */
        cancel_timeout();
        dispatch_command();
        reset_cmd();
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

    /* Register built-in commands using raw DTMF digit sequences.
     * Users dial e.g., *87# for VS (V=8, S=7 on phone keypad)
     * But we keep it simple: patterns match raw digit strings.
     * Actual mapping configured via config or documented for users. */

    /* Voicemail commands: *8<subcmd># where 8='V' group */
    register_cmd("87",  DTMF_EVT_VOICEMAIL_STATUS, "Voicemail status");
    register_cmd("86",  DTMF_EVT_VOICEMAIL_RECORD, "Voicemail record");
    register_cmd("85",  DTMF_EVT_VOICEMAIL_PLAY,   "Voicemail play");
    register_cmd("83",  DTMF_EVT_VOICEMAIL_DELETE, "Voicemail delete");
    register_cmd("84",  DTMF_EVT_VOICEMAIL_LIST,   "Voicemail list");

    /* GPIO: *41<pin># = on, *40<pin># = off (4='G' group) */
    register_cmd("41",  DTMF_EVT_GPIO_ON,  "GPIO on");
    register_cmd("40",  DTMF_EVT_GPIO_OFF, "GPIO off");

    /* Weather: *93# current, *94# forecast (9='W' group) */
    register_cmd("93",  8, "Weather report");
    register_cmd("94",  9, "Weather forecast");

    /* Time: *95# */
    register_cmd("95", 10, "Time check");

    /* Emergency: *911# on, *910# off */
    register_cmd("911", 11, "Emergency on");
    register_cmd("910", 12, "Emergency off");

    /* Parrot/echo */
    register_cmd("88", 13, "Parrot echo");

    /* NWS alerts */
    register_cmd("96", 14, "NWS alerts");

    /* OTP authentication */
    register_cmd("68", 15, "OTP authenticate");

    core->subscribe(KERCHEVT_DTMF_DIGIT, on_dtmf_digit, NULL);
    core->subscribe(KERCHEVT_COR_ASSERT, on_cor_event, NULL);
    core->subscribe(KERCHEVT_COR_DROP, on_cor_event, NULL);
    return 0;
}

static int dtmfcmd_configure(const kerchunk_config_t *cfg)
{
    g_timeout_ms = kerchunk_config_get_int(cfg, "dtmf", "inter_digit_timeout", 3000);
    g_cor_gate_ms = kerchunk_config_get_int(cfg, "dtmf", "cor_gate_ms", 200);
    return 0;
}

static void dtmfcmd_unload(void)
{
    g_core->unsubscribe(KERCHEVT_DTMF_DIGIT, on_dtmf_digit);
    g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_event);
    g_core->unsubscribe(KERCHEVT_COR_DROP, on_cor_event);
    cancel_timeout();
    if (g_cor_gate_timer >= 0) {
        g_core->timer_cancel(g_cor_gate_timer);
        g_cor_gate_timer = -1;
    }
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
        char frag[5440];
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
        char line[5440];
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
