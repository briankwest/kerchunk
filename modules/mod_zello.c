/*
 * mod_zello.c — Zello Channel API bridge
 *
 * Runs a libzello client inside kerchunk, connecting to a Zello channel
 * (Friends & Family or Zello Work) and bridging audio between Zello and
 * RF.
 *
 * Audio profile is libzello's default: Opus mono, 16 kHz, 60 ms frames.
 * The kerchunk audio thread runs at 48 kHz, so this module resamples
 * 48k↔16k via kerchunk_resample_into() in both directions.
 *
 * Config section [zello]:
 *   enabled         = yes
 *   server_url      = wss://zello.io/ws         ; F&F default; for Work use
 *                                                ; wss://zellowork.io/ws/<network>
 *   username        = kerchunkd
 *   password        = ...                       ; consumer Zello account password
 *   channel         = mychannel
 *   auth_token      = eyJ...                    ; JWT from developers.zello.com
 *   auth_token_file = /etc/kerchunk/zello.jwt   ; alternative — JWT loaded from file
 *   listen_only     = no
 *   rf_to_zello     = yes                       ; forward RF RX audio to Zello
 *   zello_to_rf     = yes                       ; forward Zello audio to RF TX
 *   priority        = 3                         ; TX queue priority for Zello→RF audio
 *   virtual_user_id = 998                       ; kerchunk user_id stamped on VCOR
 *                                                ; events for inbound Zello audio
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "kerchunk_queue.h"
#include "kerchunk_wav.h"             /* kerchunk_resample_into */
#include <libzello/zello_client.h>
#include <libzello/zello.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MOD "zello"

static kerchunk_core_t *g_core;
static zello_client_t  *g_cli;
static int              g_poll_timer = -1;

/* ── Config ────────────────────────────────────────────────────── */

static int   g_enabled;
static char  g_server_url[256];
static char  g_username[64];
static char  g_password[128];
static char  g_channel[128];
static char *g_auth_token;          /* heap-allocated — may be long */
static int   g_listen_only;
static int   g_rf_to_zello;
static int   g_zello_to_rf;
static int   g_priority;
static int   g_virtual_user_id;

/* ── State ─────────────────────────────────────────────────────── */

static int g_rf_rx_active;          /* RF COR is asserted; we're TX'ing to Zello */
static int g_zello_rx_active;       /* a remote Zello user is talking */

/* ── Audio rates ───────────────────────────────────────────────── */

#define ZELLO_RATE             16000
#define KERCHUNK_RATE          48000
#define KERCHUNK_FRAME_SAMPLES 960     /* 20 ms @ 48 kHz */
#define ZELLO_FRAME_SAMPLES_20MS 320   /* 20 ms @ 16 kHz — what we feed libzello */

/* ── libzello log bridge ───────────────────────────────────────── */

static void zello_log_bridge(int level, const char *msg, void *ud)
{
    (void)ud;
    int klevel;
    switch (level) {
    case ZELLO_LOG_ERROR: klevel = KERCHUNK_LOG_ERROR; break;
    case ZELLO_LOG_WARN:  klevel = KERCHUNK_LOG_WARN;  break;
    case ZELLO_LOG_INFO:  klevel = KERCHUNK_LOG_INFO;  break;
    case ZELLO_LOG_DEBUG: klevel = KERCHUNK_LOG_DEBUG; break;
    default:              klevel = KERCHUNK_LOG_DEBUG; break;
    }
    g_core->log(klevel, LOG_MOD, "%s", msg);
}

/* ── libzello → kerchunk callbacks ─────────────────────────────── */

static void on_zello_connected(zello_client_t *c, const char *refresh_token, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "channel ready (refresh_token=%s)",
                refresh_token ? "captured" : "none");
}

static void on_zello_disconnected(zello_client_t *c, int code,
                                   const char *reason, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "disconnected (code=%d reason=%s)", code, reason ? reason : "");
    /* libzello schedules its own reconnect; release any kerchunk PTT
     * we were holding so the audio path doesn't get stuck. */
    if (g_zello_rx_active) {
        g_zello_rx_active = 0;
        g_core->release_ptt("zello");
        kerchevt_t vc = { .type = KERCHEVT_VCOR_DROP,
            .vcor = { .source = "zello", .user_id = g_virtual_user_id } };
        kerchevt_fire(&vc);
    }
}

static void on_zello_channel_status(zello_client_t *c, const char *chan,
                                     bool online, int users, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "channel '%s' %s (%d users online)",
                chan ? chan : "?", online ? "online" : "offline", users);
}

static void on_zello_stream_start(zello_client_t *c, uint32_t sid,
                                   const char *from, int sr, int frame_ms,
                                   void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "stream %u from=%s sr=%d frame=%dms",
                sid, from ? from : "?", sr, frame_ms);

    if (!g_zello_to_rf) return;

    /* Same shape as mod_poc: hold kerchunk PTT for the whole Zello
     * transmission, then drop. Without this the queue auto-PTT
     * cycles per frame and chops the audio. */
    g_zello_rx_active = 1;
    g_core->request_ptt("zello");

    /* Virtual COR — feeds mod_recorder / mod_courtesy / mod_asr / mod_cdr
     * the same way RF COR does. virtual_user_id maps to a user record in
     * the kerchunk DB so recorders/transcripts can attribute correctly. */
    kerchevt_t vc = { .type = KERCHEVT_VCOR_ASSERT,
        .vcor = { .source = "zello", .user_id = g_virtual_user_id } };
    kerchevt_fire(&vc);
}

static void on_zello_audio(zello_client_t *c, uint32_t sid,
                            const int16_t *pcm, size_t n_samples,
                            int sample_rate, void *ud)
{
    (void)c; (void)sid; (void)ud;
    if (!g_zello_to_rf || !g_zello_rx_active) return;

    /* Resample Zello rate → 48 kHz. At the default 16 kHz, 60 ms = 960
     * samples in, 2880 out — well within the typical PCM stack alloc. */
    size_t out_max = n_samples * KERCHUNK_RATE / sample_rate + 16;
    int16_t *upsampled = malloc(out_max * sizeof(int16_t));
    if (!upsampled) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "OOM resampling Zello audio");
        return;
    }
    size_t up_n = kerchunk_resample_into(upsampled, out_max,
                                          pcm, n_samples,
                                          sample_rate, KERCHUNK_RATE);
    kerchunk_queue_add_buffer_src(upsampled, (int)up_n, g_priority,
                                   QUEUE_FLAG_NO_TAIL, "zello");
    free(upsampled);
}

static void on_zello_stream_stop(zello_client_t *c, uint32_t sid, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "stream %u stop", sid);
    if (g_zello_rx_active) {
        g_zello_rx_active = 0;
        g_core->release_ptt("zello");
        kerchevt_t vc = { .type = KERCHEVT_VCOR_DROP,
            .vcor = { .source = "zello", .user_id = g_virtual_user_id } };
        kerchevt_fire(&vc);
    }
}

static void on_zello_text(zello_client_t *c, const char *from,
                           const char *text, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "text from %s: %s", from ? from : "?", text ? text : "");
}

static void on_zello_error(zello_client_t *c, const char *code, void *ud)
{
    (void)c; (void)ud;
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "server error: %s",
                code ? code : "?");
}

/* ── kerchunk event handlers ───────────────────────────────────── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_rf_to_zello || !g_cli) return;
    if (zello_client_state(g_cli) != ZELLO_STATE_ONLINE) return;

    g_rf_rx_active = 1;
    if (zello_client_start_tx(g_cli) != ZELLO_OK) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "start_tx failed");
        g_rf_rx_active = 0;
    }
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_rf_rx_active || !g_cli) return;
    g_rf_rx_active = 0;
    zello_client_stop_tx(g_cli);
}

static void on_audio_frame(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_rf_to_zello || !g_rf_rx_active || !g_cli) return;

    /* Resample 48 kHz → 16 kHz (kerchunk audio → libzello encoder rate). */
    int16_t pcm16k[ZELLO_FRAME_SAMPLES_20MS + 16];
    size_t out_n = kerchunk_resample_into(pcm16k, sizeof(pcm16k)/sizeof(pcm16k[0]),
                                           evt->audio.samples, evt->audio.n,
                                           KERCHUNK_RATE, ZELLO_RATE);
    zello_client_send_pcm(g_cli, pcm16k, out_n);
}

static void on_shutdown(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_cli) {
        zello_client_stop(g_cli);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "client stopped");
    }
}

static void on_config_reload(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "config reload requested");
    /* Full reload happens via configure(); the core invokes it for us. */
}

/* ── Poll timer ────────────────────────────────────────────────── */

static void poll_timer_cb(void *ud)
{
    (void)ud;
    if (g_cli) zello_client_poll(g_cli, 0);
}

/* ── Helpers ───────────────────────────────────────────────────── */

/* Load a JWT (or any string blob) from a file. Returns malloc'd string
 * with trailing whitespace stripped. */
static char *load_text_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || n > 1024 * 1024) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    /* Trim trailing whitespace/newlines */
    while (got > 0 && (buf[got - 1] == '\n' || buf[got - 1] == '\r' ||
                       buf[got - 1] == ' '  || buf[got - 1] == '\t')) {
        buf[--got] = 0;
    }
    return buf;
}

/* ── CLI commands ──────────────────────────────────────────────── */

static const char *state_name(zello_state_t s)
{
    switch (s) {
    case ZELLO_STATE_OFFLINE:    return "offline";
    case ZELLO_STATE_CONNECTING: return "connecting";
    case ZELLO_STATE_LOGON:      return "logon";
    case ZELLO_STATE_ONLINE:     return "online";
    case ZELLO_STATE_RECONNECT:  return "reconnect";
    }
    return "?";
}

static int cli_zello(int argc, const char **argv, kerchunk_resp_t *resp)
{
    resp_init(resp);

    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        const char *st = g_cli ? state_name(zello_client_state(g_cli)) : "unloaded";
        resp_str(resp, "state",         st);
        resp_str(resp, "server",        g_server_url);
        resp_str(resp, "channel",       g_channel);
        resp_str(resp, "username",      g_username);
        resp_bool(resp, "listen_only",  g_listen_only);
        resp_bool(resp, "rf_to_zello",  g_rf_to_zello);
        resp_bool(resp, "zello_to_rf",  g_zello_to_rf);
        resp_bool(resp, "rf_rx_active", g_rf_rx_active);
        resp_bool(resp, "zello_rx_active", g_zello_rx_active);
        char buf[640];
        snprintf(buf, sizeof(buf),
                 "Zello: %s, server=%s, channel=%s, user=%s%s",
                 st, g_server_url, g_channel, g_username,
                 g_listen_only ? " (listen-only)" : "");
        resp_text_raw(resp, buf);
        resp_finish(resp);
        return 0;
    }

    if (strcmp(argv[1], "connect") == 0) {
        if (!g_cli) {
            resp_text_raw(resp, "zello not configured");
            resp_finish(resp);
            return -1;
        }
        int rc = zello_client_start(g_cli);
        char buf[64];
        snprintf(buf, sizeof(buf), "connect: %s", rc == ZELLO_OK ? "ok" : "failed");
        resp_text_raw(resp, buf);
        resp_bool(resp, "ok", rc == ZELLO_OK);
        resp_finish(resp);
        return rc;
    }

    if (strcmp(argv[1], "disconnect") == 0) {
        if (!g_cli) {
            resp_text_raw(resp, "zello not configured");
            resp_finish(resp);
            return -1;
        }
        zello_client_stop(g_cli);
        resp_text_raw(resp, "disconnected");
        resp_bool(resp, "ok", 1);
        resp_finish(resp);
        return 0;
    }

    if (strcmp(argv[1], "say") == 0) {
        if (argc < 3) {
            resp_text_raw(resp, "Usage: zello say <text>");
            resp_finish(resp);
            return -1;
        }
        if (!g_cli) {
            resp_text_raw(resp, "zello not configured");
            resp_finish(resp);
            return -1;
        }
        char msg[512] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
        }
        int rc = zello_client_send_text(g_cli, msg);
        char buf[64];
        snprintf(buf, sizeof(buf), "say: %s", rc == ZELLO_OK ? "sent" : "failed");
        resp_text_raw(resp, buf);
        resp_bool(resp, "ok", rc == ZELLO_OK);
        resp_finish(resp);
        return rc;
    }

    resp_text_raw(resp, "Usage: zello [status|connect|disconnect|say <text>]");
    resp_finish(resp);
    return -1;
}

static const kerchunk_ui_field_t say_fields[] = {
    { "text", "Message", "text", NULL, "Hello from kerchunk" },
};

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "zello", "zello [status|connect|disconnect|say]",
      "Zello channel bridge management", cli_zello,
      .category = "Zello", .ui_label = "Status",
      .ui_type = CLI_UI_BUTTON, .ui_command = "zello status",
      .subcommands = "status,connect,disconnect,say" },
    { "zello connect", "zello connect",
      "Connect to the Zello channel", cli_zello,
      .category = "Zello", .ui_label = "Connect",
      .ui_type = CLI_UI_BUTTON, .ui_command = "zello connect" },
    { "zello disconnect", "zello disconnect",
      "Disconnect from the Zello channel", cli_zello,
      .category = "Zello", .ui_label = "Disconnect",
      .ui_type = CLI_UI_BUTTON, .ui_command = "zello disconnect" },
    { "zello say", "zello say <text>",
      "Send a text message to the channel", cli_zello,
      .category = "Zello", .ui_label = "Say",
      .ui_type = CLI_UI_FORM, .ui_command = "zello say",
      .ui_fields = say_fields, .num_ui_fields = 1 },
};

/* ── Module lifecycle ──────────────────────────────────────────── */

/* Tear down the live client + timer. Does NOT free g_auth_token —
 * that's a config-owned value freed/reloaded at the top of
 * mod_configure() (so callers can safely invoke teardown after
 * loading a new token). */
static void teardown_client(void)
{
    if (g_poll_timer >= 0) {
        g_core->timer_cancel(g_poll_timer);
        g_poll_timer = -1;
    }
    if (g_cli) {
        zello_client_destroy(g_cli);
        g_cli = NULL;
    }
    g_rf_rx_active = 0;
    g_zello_rx_active = 0;
}

static int mod_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_COR_ASSERT,    on_cor_assert,    NULL);
    core->subscribe(KERCHEVT_COR_DROP,      on_cor_drop,      NULL);
    core->subscribe(KERCHEVT_AUDIO_FRAME,   on_audio_frame,   NULL);
    core->subscribe(KERCHEVT_SHUTDOWN,      on_shutdown,      NULL);
    core->subscribe(KERCHEVT_CONFIG_RELOAD, on_config_reload, NULL);
    return 0;
}

static int mod_configure(const kerchunk_config_t *cfg)
{
    (void)cfg;

    g_enabled = g_core->config_get_int("zello", "enabled", 0);
    if (!g_enabled) {
        teardown_client();
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled by config");
        return 0;
    }

    const char *server_url = g_core->config_get("zello", "server_url");
    snprintf(g_server_url, sizeof(g_server_url), "%s",
             server_url ? server_url : "wss://zello.io/ws");

    const char *username = g_core->config_get("zello", "username");
    const char *password = g_core->config_get("zello", "password");
    const char *channel  = g_core->config_get("zello", "channel");
    if (!username || !channel) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "missing required config: username and channel");
        return -1;
    }
    snprintf(g_username, sizeof(g_username), "%s", username);
    snprintf(g_password, sizeof(g_password), "%s", password ? password : "");
    snprintf(g_channel,  sizeof(g_channel),  "%s", channel);

    g_listen_only     = g_core->config_get_int("zello", "listen_only", 0);
    g_rf_to_zello     = g_core->config_get_int("zello", "rf_to_zello", 1);
    g_zello_to_rf     = g_core->config_get_int("zello", "zello_to_rf", 1);
    g_priority        = g_core->config_get_int("zello", "priority", KERCHUNK_PRI_NORMAL);
    g_virtual_user_id = g_core->config_get_int("zello", "virtual_user_id", 998);

    /* JWT: prefer file (more practical for 500B+ tokens). */
    if (g_auth_token) { free(g_auth_token); g_auth_token = NULL; }
    const char *tok_file = g_core->config_get("zello", "auth_token_file");
    const char *tok_inline = g_core->config_get("zello", "auth_token");
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "auth_token_file='%s' auth_token_inline=%s",
                tok_file ? tok_file : "(null)",
                tok_inline && *tok_inline ? "(set)" : "(empty)");
    if (tok_file && *tok_file) {
        g_auth_token = load_text_file(tok_file);
        if (!g_auth_token) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "auth_token_file '%s' unreadable — Zello F&F will reject logon", tok_file);
        } else {
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "auth_token loaded from %s (%zu bytes)",
                        tok_file, strlen(g_auth_token));
        }
    } else if (tok_inline && *tok_inline) {
        g_auth_token = strdup(tok_inline);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "auth_token loaded inline (%zu bytes)", strlen(g_auth_token));
    } else {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "no auth_token configured — Zello F&F logon will fail");
    }

    /* Tear down existing client on reconfigure. */
    teardown_client();

    /* Wire libzello logging into the kerchunk logger. */
    zello_set_log_callback(zello_log_bridge, NULL);
    zello_set_log_level(ZELLO_LOG_INFO);

    zello_config_t zcfg = {
        .server_url  = g_server_url,
        .username    = g_username,
        .password    = g_password[0] ? g_password : NULL,
        .channel     = g_channel,
        .auth_token  = g_auth_token,
        .listen_only = g_listen_only,
    };
    zello_callbacks_t cb = {
        .on_connected      = on_zello_connected,
        .on_disconnected   = on_zello_disconnected,
        .on_channel_status = on_zello_channel_status,
        .on_stream_start   = on_zello_stream_start,
        .on_audio          = on_zello_audio,
        .on_stream_stop    = on_zello_stream_stop,
        .on_text_message   = on_zello_text,
        .on_error          = on_zello_error,
    };

    g_cli = zello_client_create(&zcfg, &cb);
    if (!g_cli) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "zello_client_create failed (check username/channel)");
        return -1;
    }

    int rc = zello_client_start(g_cli);
    if (rc != ZELLO_OK) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "zello_client_start returned %d — will retry via reconnect", rc);
    }

    g_poll_timer = g_core->timer_create(10, 1, poll_timer_cb, NULL);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "client started: server=%s channel=%s user=%s%s",
                g_server_url, g_channel, g_username,
                g_listen_only ? " (listen-only)" : "");
    return 0;
}

static void mod_unload(void)
{
    teardown_client();
    if (g_auth_token) { free(g_auth_token); g_auth_token = NULL; }

    g_core->unsubscribe(KERCHEVT_COR_ASSERT,    on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,      on_cor_drop);
    g_core->unsubscribe(KERCHEVT_AUDIO_FRAME,   on_audio_frame);
    g_core->unsubscribe(KERCHEVT_SHUTDOWN,      on_shutdown);
    g_core->unsubscribe(KERCHEVT_CONFIG_RELOAD, on_config_reload);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded");
}

static const kerchunk_module_def_t mod_def = {
    .name             = "mod_zello",
    .version          = "0.1.0",
    .description      = "Zello Channel API bridge",
    .load             = mod_load,
    .configure        = mod_configure,
    .unload           = mod_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_def);
