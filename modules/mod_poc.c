/*
 * mod_poc.c — PoC radio server bridge
 *
 * Runs a poc_server_t inside kerchunk, allowing PoC radios (Retevis L71,
 * TYT, etc.) to connect directly. Maps kerchunk users and groups to PoC
 * protocol equivalents. Bridges audio between the PoC network and RF.
 *
 * Config section [poc]:
 *   enabled           = yes
 *   bind              = 0.0.0.0
 *   port              = 29999
 *   tls_cert          = /etc/kerchunk/poc.crt   (optional, enables TLS)
 *   tls_key           = /etc/kerchunk/poc.key
 *   rf_bridge_group   = 1       ; kerchunk group ID bridged to RF (0=none)
 *   rf_to_poc         = yes     ; forward RF RX audio to PoC clients
 *   poc_to_rf         = yes     ; forward PoC client audio to RF TX
 *   priority          = 3       ; TX queue priority for PoC→RF audio
 *   virtual_user_id   = 999     ; sender ID for RF audio in PoC
 *
 * PoC credentials in users.conf (per user):
 *   [user.N]
 *   poc_password = <password>   ; enables PoC access for this user
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "kerchunk_queue.h"
#include <libpoc/poc.h>
#include <libpoc/poc_server.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LOG_MOD "poc"

static kerchunk_core_t *g_core;
static poc_server_t    *g_srv;
static int              g_poll_timer = -1;

/* ── Config ────────────────────────────────────────────────────── */

static int      g_enabled;
static int      g_port;
static char     g_bind[64];
static char     g_tls_cert[256];
static char     g_tls_key[256];
static uint32_t g_rf_bridge_group;
static int      g_rf_to_poc;
static int      g_poc_to_rf;
static int      g_priority;
static uint32_t g_virtual_user_id;

/* ── State ─────────────────────────────────────────────────────── */

static int      g_rf_rx_active;       /* COR is asserted */
static int      g_poc_ptt_active;     /* a PoC client is transmitting */
static uint32_t g_poc_ptt_speaker;    /* who holds the floor */
static int      g_audio_frame_count;  /* frames received this PTT session */

/* ── Resampling ────────────────────────────────────────────────── */

static void downsample_48_to_8(const int16_t *in, int in_count,
                               int16_t *out, int out_count)
{
    for (int i = 0; i < out_count && i * 6 < in_count; i++)
        out[i] = in[i * 6];
}

static void upsample_8_to_48(const int16_t *in, int in_count,
                              int16_t *out, int out_count)
{
    for (int i = 0; i < out_count; i++) {
        int src_idx = i / 6;
        int frac = i % 6;
        if (src_idx + 1 < in_count) {
            int32_t a = in[src_idx];
            int32_t b = in[src_idx + 1];
            out[i] = (int16_t)(a + (b - a) * frac / 6);
        } else {
            out[i] = in[in_count - 1];
        }
    }
}

/* ── libpoc log bridge ─────────────────────────────────────────── */

static void poc_log_bridge(int level, const char *msg, void *ud)
{
    (void)ud;
    int klevel;
    switch (level) {
    case POC_LOG_ERROR:   klevel = KERCHUNK_LOG_ERROR; break;
    case POC_LOG_WARNING: klevel = KERCHUNK_LOG_WARN; break;
    case POC_LOG_INFO:    klevel = KERCHUNK_LOG_INFO; break;
    case POC_LOG_DEBUG:   klevel = KERCHUNK_LOG_DEBUG; break;
    default:              klevel = KERCHUNK_LOG_DEBUG; break;
    }
    g_core->log(klevel, LOG_MOD, "%s", msg);
}

/* ── Server callbacks ──────────────────────────────────────────── */

static void poc_on_connect(poc_server_t *srv, uint32_t uid,
                           const char *account, void *ud)
{
    (void)srv; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "%s (user %u) connected", account, uid);
}

static void poc_on_disconnect(poc_server_t *srv, uint32_t uid,
                              const char *account, void *ud)
{
    (void)srv; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "%s (user %u) disconnected", account, uid);

    if (uid == g_poc_ptt_speaker) {
        g_poc_ptt_active = 0;
        g_poc_ptt_speaker = 0;
    }
}

static bool poc_on_ptt_request(poc_server_t *srv, uint32_t uid,
                               uint32_t gid, void *ud)
{
    (void)srv; (void)ud;

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "PTT request from user %u group %u (rf_busy=%d)", uid, gid, g_rf_rx_active);

    /* Deny if RF channel is busy and this is the bridge group */
    if (gid == g_rf_bridge_group && g_rf_rx_active) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "PTT denied for user %u — RF channel busy", uid);
        return false;
    }

    g_poc_ptt_active = 1;
    g_poc_ptt_speaker = uid;
    g_audio_frame_count = 0;

    /* Fire virtual COR so ASR/recorder treat this like RF */
    kerchevt_t vc = { .type = KERCHEVT_VCOR_ASSERT,
        .vcor = { .source = "poc", .user_id = (int)uid } };
    kerchevt_fire(&vc);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "PTT granted to user %u on group %u", uid, gid);
    return true;
}

static void poc_on_ptt_end(poc_server_t *srv, uint32_t uid,
                           uint32_t gid, void *ud)
{
    (void)srv; (void)gid; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "PTT end from user %u (%d audio frames received)", uid, g_audio_frame_count);
    if (uid == g_poc_ptt_speaker) {
        g_poc_ptt_active = 0;
        g_poc_ptt_speaker = 0;

        kerchevt_t vc = { .type = KERCHEVT_VCOR_DROP,
            .vcor = { .source = "poc", .user_id = (int)uid } };
        kerchevt_fire(&vc);
    }
}

static void poc_on_audio(poc_server_t *srv, uint32_t speaker_id,
                         uint32_t gid, const int16_t *pcm,
                         int n_samples, void *ud)
{
    (void)srv; (void)ud;

    g_audio_frame_count++;
    if (g_audio_frame_count <= 3 || (g_audio_frame_count % 50) == 0)
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "audio frame #%d from user %u group %u (%d samples) poc_to_rf=%d bridge_group=%u",
                    g_audio_frame_count, speaker_id, gid, n_samples,
                    g_poc_to_rf, g_rf_bridge_group);

    if (!g_poc_to_rf || gid != g_rf_bridge_group) {
        if (g_audio_frame_count <= 3)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "audio DROPPED: poc_to_rf=%d gid=%u bridge=%u",
                        g_poc_to_rf, gid, g_rf_bridge_group);
        return;
    }

    /* Upsample 8kHz → 48kHz */
    int16_t upsampled[960];
    upsample_8_to_48(pcm, n_samples, upsampled, 960);

    kerchunk_queue_add_buffer_src(upsampled, 960, g_priority,
                                  QUEUE_FLAG_NO_TAIL, "poc");
}

static void poc_on_message(poc_server_t *srv, uint32_t from,
                           uint32_t target, const char *text, void *ud)
{
    (void)srv; (void)target; (void)ud;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "message from user %u to %u: %s", from, target, text);
}

static void poc_on_sos(poc_server_t *srv, uint32_t uid,
                       int alert_type, void *ud)
{
    (void)srv; (void)ud;
    if (alert_type < 0) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "SOS CANCELLED by user %u", uid);
        kerchunk_core_set_emergency(0);
        return;
    }
    const char *names[] = {"SOS", "ManDown", "Fall", "CallAlarm"};
    g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                "*** %s from user %u ***",
                alert_type < 4 ? names[alert_type] : "ALERT", uid);
    kerchunk_core_set_emergency(1);
}

static void poc_on_group_enter(poc_server_t *srv, uint32_t uid,
                               uint32_t gid, void *ud)
{
    (void)srv; (void)ud;
    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                "user %u joined group %u", uid, gid);
}

static void poc_on_group_leave(poc_server_t *srv, uint32_t uid,
                               uint32_t gid, void *ud)
{
    (void)srv; (void)ud;
    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                "user %u left group %u", uid, gid);
}

/* ── Kerchunk event handlers ───────────────────────────────────── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_rf_to_poc || !g_srv || !g_rf_bridge_group)
        return;

    g_rf_rx_active = 1;
    poc_server_start_ptt_for(g_srv, g_rf_bridge_group,
                             g_virtual_user_id, "Repeater");
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_rf_rx_active || !g_srv)
        return;

    g_rf_rx_active = 0;
    poc_server_end_ptt_for(g_srv, g_rf_bridge_group,
                           g_virtual_user_id);
}

static void on_audio_frame(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_rf_to_poc || !g_rf_rx_active || !g_srv || !g_rf_bridge_group)
        return;

    const int16_t *samples = evt->audio.samples;
    int count = (int)evt->audio.n;

    /* Downsample 48kHz → 8kHz */
    int16_t pcm8k[160];
    int out_count = count / 6;
    if (out_count > 160) out_count = 160;
    downsample_48_to_8(samples, count, pcm8k, out_count);

    poc_server_inject_audio(g_srv, g_rf_bridge_group,
                            g_virtual_user_id, pcm8k, out_count);
}

static void on_shutdown(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_srv) {
        poc_server_stop(g_srv);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "server stopped");
    }
}

static void on_config_reload(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    /* TODO: re-scan users for poc_password changes, add/remove users */
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "config reload (user resync pending)");
}

/* ── Poll timer ────────────────────────────────────────────────── */

static void poll_timer_cb(void *ud)
{
    (void)ud;
    if (g_srv)
        poc_server_poll(g_srv, 0);
}

/* ── User/group sync ───────────────────────────────────────────── */

static void sync_users(void)
{
    kerchunk_config_t *ucfg = kerchunk_core_get_users_config();
    if (!ucfg) ucfg = kerchunk_core_get_config();

    int added = 0;
    int iter = 0;
    const char *section;
    while ((section = kerchunk_config_next_section(ucfg, &iter)) != NULL) {
        if (strncmp(section, "user.", 5) != 0) continue;

        const char *poc_pw = kerchunk_config_get(ucfg, section, "poc_password");
        if (!poc_pw || !poc_pw[0]) continue;

        const char *username = kerchunk_config_get(ucfg, section, "username");
        const char *fullname = kerchunk_config_get(ucfg, section, "name");
        if (!username) {
            username = fullname;
            if (!username) continue;
        }

        int user_id = atoi(section + 5);  /* "user.N" → N */
        if (user_id <= 0) continue;

        poc_server_add_user(g_srv, &(poc_server_user_t){
            .account  = username,
            .name     = fullname ? fullname : username,
            .password = poc_pw,
            .user_id  = (uint32_t)(user_id + 1000),
        });
        added++;

        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                    "added PoC user: %s '%s' (id %d)", username,
                    fullname ? fullname : username, user_id + 1000);
    }
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "%d PoC users configured", added);
}

static void sync_groups(void)
{
    int n = kerchunk_group_count();
    for (int i = 0; i < n; i++) {
        const kerchunk_group_t *g = kerchunk_group_get(i);
        if (!g) continue;

        poc_server_add_group(g_srv, &(poc_server_group_t){
            .id           = (uint32_t)g->id,
            .name         = g->name,
            .member_ids   = NULL,   /* open — any authenticated user */
            .member_count = 0,
        });

        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                    "added PoC group: %d '%s'", g->id, g->name);
    }
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "%d PoC groups configured", n);
}

/* ── CLI commands ──────────────────────────────────────────────── */

static int cli_poc(int argc, const char **argv, kerchunk_resp_t *resp)
{
    resp_init(resp);

    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        /* poc / poc status */
        int online = g_srv ? poc_server_client_count(g_srv) : 0;
        resp_str(resp, "state", g_srv ? "running" : "stopped");
        resp_int(resp, "port", g_port);
        resp_int(resp, "clients", online);
        resp_int(resp, "rf_bridge_group", (int)g_rf_bridge_group);
        resp_bool(resp, "rf_rx_active", g_rf_rx_active);
        resp_bool(resp, "poc_ptt_active", g_poc_ptt_active);
        resp_int(resp, "poc_ptt_speaker", (int)g_poc_ptt_speaker);
        resp_bool(resp, "tls", g_tls_cert[0] ? 1 : 0);

        char buf[256];
        snprintf(buf, sizeof(buf), "PoC server: %s, %d clients, port %d%s",
                 g_srv ? "running" : "stopped", online, g_port,
                 g_tls_cert[0] ? " (TLS)" : "");
        resp_text_raw(resp, buf);
        resp_finish(resp);
        return 0;
    }

    if (strcmp(argv[1], "clients") == 0) {
        if (!g_srv) {
            resp_text_raw(resp, "PoC server not running");
            resp_finish(resp);
            return -1;
        }
        poc_user_t clients[64];
        int n = poc_server_get_clients(g_srv, clients, 64);
        resp_int(resp, "count", n);

        char buf[128];
        snprintf(buf, sizeof(buf), "Online clients (%d):", n);
        resp_text_raw(resp, buf);
        for (int i = 0; i < n; i++) {
            snprintf(buf, sizeof(buf), "  [%u] %s (status=%d)",
                     clients[i].id, clients[i].account, clients[i].status);
            resp_text_raw(resp, buf);
        }
        resp_finish(resp);
        return 0;
    }

    if (strcmp(argv[1], "kick") == 0) {
        if (argc < 3) {
            resp_text_raw(resp, "Usage: poc kick <user_id>");
            resp_finish(resp);
            return -1;
        }
        uint32_t uid = (uint32_t)atoi(argv[2]);
        int rc = g_srv ? poc_server_kick(g_srv, uid) : -1;
        resp_bool(resp, "ok", rc == 0);
        char buf[64];
        snprintf(buf, sizeof(buf), "Kick user %u: %s", uid, rc == 0 ? "ok" : "failed");
        resp_text_raw(resp, buf);
        resp_finish(resp);
        return rc;
    }

    if (strcmp(argv[1], "broadcast") == 0) {
        if (argc < 3) {
            resp_text_raw(resp, "Usage: poc broadcast <text>");
            resp_finish(resp);
            return -1;
        }
        /* Join remaining args into message */
        char msg[256] = "";
        for (int i = 2; i < argc; i++) {
            if (i > 2) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
        }
        int rc = g_srv ? poc_server_broadcast(g_srv, msg) : -1;
        resp_bool(resp, "ok", rc == 0);
        resp_text_raw(resp, rc == 0 ? "Broadcast sent" : "Broadcast failed");
        resp_finish(resp);
        return rc;
    }

    if (strcmp(argv[1], "msg") == 0) {
        if (argc < 4) {
            resp_text_raw(resp, "Usage: poc msg <user_id> <text>");
            resp_finish(resp);
            return -1;
        }
        uint32_t uid = (uint32_t)atoi(argv[2]);
        char msg[256] = "";
        for (int i = 3; i < argc; i++) {
            if (i > 3) strncat(msg, " ", sizeof(msg) - strlen(msg) - 1);
            strncat(msg, argv[i], sizeof(msg) - strlen(msg) - 1);
        }
        int rc = g_srv ? poc_server_send_message(g_srv, 0, uid, msg) : -1;
        char buf[64];
        snprintf(buf, sizeof(buf), "Message to %u: %s", uid, rc == 0 ? "sent" : "failed");
        resp_text_raw(resp, buf);
        resp_bool(resp, "ok", rc == 0);
        resp_finish(resp);
        return rc;
    }

    resp_text_raw(resp, "Usage: poc [status|clients|kick|broadcast|msg]");
    resp_finish(resp);
    return -1;
}

static const kerchunk_ui_field_t kick_fields[] = {
    { "user_id", "User ID", "number", NULL, "1001" },
};

static const kerchunk_ui_field_t msg_fields[] = {
    { "user_id", "User ID", "number", NULL, "1001" },
    { "text", "Message", "text", NULL, "Hello" },
};

static const kerchunk_ui_field_t bcast_fields[] = {
    { "text", "Message", "text", NULL, "Attention all users" },
};

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "poc", "poc [status|clients|kick|broadcast|msg]",
      "PoC server management", cli_poc,
      .category = "PoC", .ui_label = "Status",
      .ui_type = CLI_UI_BUTTON, .ui_command = "poc status" },
    { "poc clients", "poc clients",
      "List connected PoC clients", cli_poc,
      .category = "PoC", .ui_label = "Clients",
      .ui_type = CLI_UI_BUTTON, .ui_command = "poc clients" },
    { "poc kick", "poc kick <user_id>",
      "Disconnect a PoC client", cli_poc,
      .category = "PoC", .ui_label = "Kick",
      .ui_type = CLI_UI_FORM, .ui_command = "poc kick",
      .ui_fields = kick_fields, .num_ui_fields = 1 },
    { "poc broadcast", "poc broadcast <text>",
      "Message all connected clients", cli_poc,
      .category = "PoC", .ui_label = "Broadcast",
      .ui_type = CLI_UI_FORM, .ui_command = "poc broadcast",
      .ui_fields = bcast_fields, .num_ui_fields = 1 },
    { "poc msg", "poc msg <user_id> <text>",
      "Message a specific client", cli_poc,
      .category = "PoC", .ui_label = "Message",
      .ui_type = CLI_UI_FORM, .ui_command = "poc msg",
      .ui_fields = msg_fields, .num_ui_fields = 2 },
};

/* ── Module lifecycle ──────────────────────────────────────────── */

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

    g_enabled = g_core->config_get_int("poc", "enabled", 1);
    if (!g_enabled) {
        /* Tear down if previously running */
        if (g_poll_timer >= 0) {
            g_core->timer_cancel(g_poll_timer);
            g_poll_timer = -1;
        }
        if (g_srv) {
            poc_server_stop(g_srv);
            poc_server_destroy(g_srv);
            g_srv = NULL;
        }
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled by config");
        return 0;
    }

    /* Read config */
    const char *bind = g_core->config_get("poc", "bind");
    snprintf(g_bind, sizeof(g_bind), "%s", bind ? bind : "0.0.0.0");

    g_port             = g_core->config_get_int("poc", "port", 29999);
    g_rf_bridge_group  = (uint32_t)g_core->config_get_int("poc", "rf_bridge_group", 0);
    g_rf_to_poc        = g_core->config_get_int("poc", "rf_to_poc", 1);
    g_poc_to_rf        = g_core->config_get_int("poc", "poc_to_rf", 1);
    g_priority         = g_core->config_get_int("poc", "priority", KERCHUNK_PRI_NORMAL);
    g_virtual_user_id  = (uint32_t)g_core->config_get_int("poc", "virtual_user_id", 999);

    const char *cert = g_core->config_get("poc", "tls_cert");
    const char *key  = g_core->config_get("poc", "tls_key");
    if (cert) snprintf(g_tls_cert, sizeof(g_tls_cert), "%s", cert);
    else g_tls_cert[0] = '\0';
    if (key) snprintf(g_tls_key, sizeof(g_tls_key), "%s", key);
    else g_tls_key[0] = '\0';

    /* Tear down existing server on reconfigure (e.g. SIGHUP reload) */
    if (g_poll_timer >= 0) {
        g_core->timer_cancel(g_poll_timer);
        g_poll_timer = -1;
    }
    if (g_srv) {
        poc_server_stop(g_srv);
        poc_server_destroy(g_srv);
        g_srv = NULL;
    }

    /* Wire libpoc logging into kerchunk logger */
    poc_set_log_callback(poc_log_bridge, NULL);
    poc_set_log_level(POC_LOG_DEBUG);

    /* Create server */
    poc_server_config_t scfg = {
        .bind_addr     = g_bind,
        .port          = (uint16_t)g_port,
        .max_clients   = 64,
        .tls           = (g_tls_cert[0] && g_tls_key[0]),
        .tls_cert_path = g_tls_cert[0] ? g_tls_cert : NULL,
        .tls_key_path  = g_tls_key[0] ? g_tls_key : NULL,
    };

    poc_server_callbacks_t cb = {
        .on_client_connect    = poc_on_connect,
        .on_client_disconnect = poc_on_disconnect,
        .on_ptt_request       = poc_on_ptt_request,
        .on_ptt_end           = poc_on_ptt_end,
        .on_audio             = poc_on_audio,
        .on_message           = poc_on_message,
        .on_sos               = poc_on_sos,
        .on_group_enter       = poc_on_group_enter,
        .on_group_leave       = poc_on_group_leave,
    };

    g_srv = poc_server_create(&scfg, &cb);
    if (!g_srv) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create PoC server");
        return -1;
    }

    /* Sync users and groups from kerchunk DB */
    sync_users();
    sync_groups();

    /* Start server */
    int rc = poc_server_start(g_srv);
    if (rc != POC_OK) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "failed to start PoC server on port %d (%d)", g_port, rc);
        poc_server_destroy(g_srv);
        g_srv = NULL;
        return -1;
    }

    /* Start poll timer (10ms) */
    g_poll_timer = g_core->timer_create(10, 1, poll_timer_cb, NULL);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "server started on %s:%d%s, bridge group=%u",
                g_bind, g_port,
                scfg.tls ? " (TLS)" : "",
                g_rf_bridge_group);
    return 0;
}

static void mod_unload(void)
{
    if (g_poll_timer >= 0) {
        g_core->timer_cancel(g_poll_timer);
        g_poll_timer = -1;
    }

    if (g_srv) {
        poc_server_destroy(g_srv);
        g_srv = NULL;
    }

    g_core->unsubscribe(KERCHEVT_COR_ASSERT,    on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,      on_cor_drop);
    g_core->unsubscribe(KERCHEVT_AUDIO_FRAME,   on_audio_frame);
    g_core->unsubscribe(KERCHEVT_SHUTDOWN,      on_shutdown);
    g_core->unsubscribe(KERCHEVT_CONFIG_RELOAD, on_config_reload);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "unloaded");
}

static const kerchunk_module_def_t mod_def = {
    .name             = "mod_poc",
    .version          = "2.0.0",
    .description      = "PoC radio server bridge",
    .load             = mod_load,
    .configure        = mod_configure,
    .unload           = mod_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = sizeof(cli_cmds) / sizeof(cli_cmds[0]),
};

KERCHUNK_MODULE_DEFINE(mod_def);
