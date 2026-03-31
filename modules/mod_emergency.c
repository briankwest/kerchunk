/*
 * mod_emergency.c — Emergency mode
 *
 * DTMF *911# activates, *910# deactivates (admin only).
 * While active: TOT disabled, automated announcements suppressed,
 * auto-deactivates after configurable timeout.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>

#define LOG_MOD "emergency"
#define DTMF_EVT_EMERGENCY_ON   (KERCHEVT_CUSTOM + 11)
#define DTMF_EVT_EMERGENCY_OFF  (KERCHEVT_CUSTOM + 12)

static kerchunk_core_t *g_core;

/* Config */
static int  g_timeout_ms  = 1800000;  /* 30 min auto-deactivate */
static char g_sounds_dir[256] = "/etc/kerchunk/sounds";

/* State */
static int g_timer = -1;

static void emergency_off(void)
{
    if (!kerchunk_core_get_emergency())
        return;

    kerchunk_core_set_emergency(0);

    if (g_timer >= 0) {
        g_core->timer_cancel(g_timer);
        g_timer = -1;
    }

    if (g_core->tts_speak)
        g_core->tts_speak("Emergency mode deactivated.", KERCHUNK_PRI_CRITICAL);
    else {
        char path[512];
        snprintf(path, sizeof(path), "%s/system/system_emergency_off.wav", g_sounds_dir);
        g_core->queue_audio_file(path, KERCHUNK_PRI_CRITICAL);
    }

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "emergency", .description = "emergency deactivated" } };
    kerchevt_fire(&ae);

    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "emergency mode DEACTIVATED");
}

static void timeout_cb(void *ud)
{
    (void)ud;
    g_timer = -1;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "emergency mode auto-deactivating after %ds", g_timeout_ms / 1000);
    emergency_off();
}

static void on_emergency_on(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;

    if (kerchunk_core_get_emergency()) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "already in emergency mode");
        return;
    }

    kerchunk_core_set_emergency(1);

    /* Start auto-deactivate timer */
    if (g_timer >= 0)
        g_core->timer_cancel(g_timer);
    g_timer = g_core->timer_create(g_timeout_ms, 0, timeout_cb, NULL);

    if (g_core->tts_speak)
        g_core->tts_speak("Emergency mode activated.", KERCHUNK_PRI_CRITICAL);
    else {
        char path[512];
        snprintf(path, sizeof(path), "%s/system/system_emergency_on.wav", g_sounds_dir);
        g_core->queue_audio_file(path, KERCHUNK_PRI_CRITICAL);
    }

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "emergency", .description = "emergency activated" } };
    kerchevt_fire(&ae);

    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "emergency mode ACTIVATED (timeout=%ds)", g_timeout_ms / 1000);
}

static void on_emergency_off(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    emergency_off();
}

static int emergency_load(kerchunk_core_t *core)
{
    g_core = core;

    if (core->dtmf_register) {
        core->dtmf_register("911", 11, "Emergency on",  "emergency_on");
        core->dtmf_register("910", 12, "Emergency off", "emergency_off");
    }

    core->subscribe(DTMF_EVT_EMERGENCY_ON,  on_emergency_on, NULL);
    core->subscribe(DTMF_EVT_EMERGENCY_OFF, on_emergency_off, NULL);
    return 0;
}

static int emergency_configure(const kerchunk_config_t *cfg)
{
    g_timeout_ms = kerchunk_config_get_int(cfg, "emergency", "timeout", 1800000);

    const char *v = kerchunk_config_get(cfg, "general", "sounds_dir");
    if (v) snprintf(g_sounds_dir, sizeof(g_sounds_dir), "%s", v);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "timeout=%ds", g_timeout_ms / 1000);
    return 0;
}

static void emergency_unload(void)
{
    if (g_core->dtmf_unregister) {
        g_core->dtmf_unregister("911");
        g_core->dtmf_unregister("910");
    }
    g_core->unsubscribe(DTMF_EVT_EMERGENCY_ON,  on_emergency_on);
    g_core->unsubscribe(DTMF_EVT_EMERGENCY_OFF, on_emergency_off);
    if (g_timer >= 0)
        g_core->timer_cancel(g_timer);
    kerchunk_core_set_emergency(0);
}

/* CLI */
static int cli_emergency(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    resp_bool(r, "active", kerchunk_core_get_emergency());
    resp_str(r, "mode", kerchunk_core_get_emergency() ? "ACTIVE" : "inactive");
    resp_int(r, "timeout_s", g_timeout_ms / 1000);
    return 0;

usage:
    resp_text_raw(r, "Emergency mode control\n\n"
        "  emergency\n"
        "    Show current emergency mode status.\n\n"
        "    Fields:\n"
        "      active       Whether emergency mode is currently on\n"
        "      mode         ACTIVE or inactive\n"
        "      timeout_s    Auto-deactivation timeout in seconds\n\n"
        "    DTMF *911# activates emergency mode, *910# deactivates.\n"
        "    While active: TOT is disabled, automated announcements are\n"
        "    suppressed. Auto-deactivates after the configured timeout.\n\n"
        "Config: [emergency] timeout\n"
        "DTMF:   *911# on, *910# off\n");
    resp_str(r, "error", "usage: emergency [help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "emergency", .usage = "emergency", .description = "Emergency mode status",
      .handler = cli_emergency,
      .category = "Control", .ui_label = "Emergency", .ui_type = CLI_UI_TOGGLE,
      .ui_command = "emergency" },
};

static kerchunk_module_def_t mod_emergency = {
    .name             = "mod_emergency",
    .version          = "1.0.0",
    .description      = "Emergency mode control",
    .load             = emergency_load,
    .configure        = emergency_configure,
    .unload           = emergency_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_emergency);
