/*
 * mod_web.c — Embedded HTTP server for web dashboard
 *
 * Serves static files and JSON API endpoints. SSE for live events.
 * API routes map directly to CLI handlers via kerchunk_resp_t.
 * Uses mongoose (cesanta.com) for HTTP/HTTPS and SSE.
 *
 * TLS is available via mongoose's built-in TLS implementation —
 * no external dependency required. Configure tls_cert and tls_key
 * in [web] to enable HTTPS.
 *
 * Config: [web] section in kerchunk.conf
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <fcntl.h>

#define LOG_MOD "web"

#define API_HEADERS \
    "Content-Type: application/json\r\n" \
    "Access-Control-Allow-Origin: *\r\n"

#define CORS_HEADERS \
    "Access-Control-Allow-Origin: *\r\n" \
    "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n" \
    "Access-Control-Allow-Headers: Authorization, Content-Type\r\n" \
    "Access-Control-Max-Age: 86400\r\n"

static int is_sensitive(const char *key) {
    static const char *sens[] = {"api_key","auth_token","totp_secret",
                                  "tls_key","google_maps_api_key",NULL};
    for (int i = 0; sens[i]; i++)
        if (strcmp(key, sens[i]) == 0) return 1;
    return 0;
}

static kerchunk_core_t *g_core;

/* Config */
static int  g_enabled      = 0;
static int  g_port         = 8080;
static char g_bind[64]     = "127.0.0.1";
static char g_auth_token[128] = "";
static char g_static_dir[256] = "";
static char g_tls_cert[256] = "";
static char g_tls_key[256]  = "";
static int  g_tls_active    = 0;

/* Mongoose state */
static struct mg_mgr g_mgr;
static unsigned long g_listener_id;
static pthread_t g_web_thread;
static volatile int g_running;
static atomic_int g_sse_count;

/* TLS cert/key data (read once, reused per connection) */
static struct mg_str g_cert_data;
static struct mg_str g_key_data;

/* ── Mongoose log redirect ── */

static char g_mg_log_buf[512];
static int  g_mg_log_pos;

static void mg_log_cb(char ch, void *param)
{
    (void)param;
    if (ch == '\n' || g_mg_log_pos >= (int)sizeof(g_mg_log_buf) - 1) {
        g_mg_log_buf[g_mg_log_pos] = '\0';
        if (g_mg_log_pos > 0 && g_core) {
            /* Mongoose format: "<hex_ts> <level_int> <file:line:func>  <msg>"
             * Map: MG 1=ERROR→LOG_ERROR, 2=INFO→LOG_INFO,
             *      3=DEBUG→LOG_DEBUG, 4=VERBOSE→LOG_DEBUG */
            int level = KERCHUNK_LOG_DEBUG;
            const char *msg = g_mg_log_buf;
            char *p = strchr(g_mg_log_buf, ' ');
            if (p) {
                switch (p[1]) {
                case '1': level = KERCHUNK_LOG_ERROR; break;
                case '2': level = KERCHUNK_LOG_INFO;  break;
                default:  level = KERCHUNK_LOG_DEBUG; break;
                }
                /* Skip "<hex> <level> " to get the rest */
                msg = p + 2;
                while (*msg == ' ') msg++;
            }
            g_core->log(level, LOG_MOD, "%s", msg);
        }
        g_mg_log_pos = 0;
    } else {
        g_mg_log_buf[g_mg_log_pos++] = ch;
    }
}

/* ── Auth check ── */

static int check_auth(struct mg_http_message *hm)
{
    if (g_auth_token[0] == '\0') return 1;  /* No auth configured */

    /* Check Authorization header first */
    struct mg_str *auth = mg_http_get_header(hm, "Authorization");
    if (auth) {
        char expected[160];
        int elen = snprintf(expected, sizeof(expected), "Bearer %s", g_auth_token);
        if ((int)auth->len == elen &&
            memcmp(auth->buf, expected, (size_t)elen) == 0)
            return 1;
    }

    /* Fall back to ?token= query parameter (for EventSource/SSE) */
    char token[128] = "";
    if (mg_http_get_var(&hm->query, "token", token, sizeof(token)) > 0) {
        if (strcmp(token, g_auth_token) == 0)
            return 1;
    }

    return 0;
}

/* ── API dispatch ── */

/* Map URL path to CLI command */
typedef struct {
    const char *path;
    const char *cmd;
    const char *arg;
} api_route_t;

static const api_route_t g_routes[] = {
    { "/api/status",    "status",    NULL },
    { "/api/stats",     "stats",     NULL },
    { "/api/nws",       "nws",       NULL },
    { "/api/cdr",       "cdr",       NULL },
    { "/api/cwid",      "cwid",      NULL },
    { "/api/parrot",    "parrot",    NULL },
    { "/api/recorder",  "recorder",  NULL },
    { "/api/emergency", "emergency", NULL },
    { "/api/weather",   "weather",   NULL },
    { "/api/time",      "time",      NULL },
    { "/api/tts",       "tts",       "status" },
    { "/api/modules",   "module",    "list" },
    { "/api/dtmfcmd",   "dtmfcmd",   NULL },
    { "/api/txcode",    "txcode",    NULL },
    { NULL, NULL, NULL }
};

/* Core command handler lookup (set by main.c) */
typedef int (*core_handler_t)(int argc, const char **argv, kerchunk_resp_t *r);

typedef struct {
    const char    *name;
    core_handler_t handler;
} core_cmd_lookup_t;

extern void kerchunk_socket_set_core_commands(const void *cmds, int count);

static void handle_api_get(struct mg_connection *c, struct mg_http_message *hm)
{
    for (int i = 0; g_routes[i].path; i++) {
        if (!mg_match(hm->uri, mg_str(g_routes[i].path), NULL)) continue;

        kerchunk_resp_t resp;
        resp_init(&resp);

        const char *argv[3] = { g_routes[i].cmd, g_routes[i].arg, NULL };
        int argc = g_routes[i].arg ? 2 : 1;

        kerchunk_dispatch_command(argc, argv, &resp);
        resp_finish(&resp);

        mg_http_reply(c, 200, API_HEADERS, "%s", resp.json);
        return;
    }

    mg_http_reply(c, 404, API_HEADERS, "{\"error\":\"Unknown API endpoint\"}");
}

static void handle_api_post_cmd(struct mg_connection *c,
                                 struct mg_http_message *hm)
{
    /* Parse {"cmd":"..."} from body */
    const char *p = mg_json_get_str(hm->body, "$.cmd");
    if (!p) {
        /* Fallback: manual parse */
        const char *s = hm->body.buf;
        const char *f = strstr(s, "\"cmd\":");
        if (!f) {
            mg_http_reply(c, 400, API_HEADERS,
                          "{\"error\":\"Missing cmd field\"}");
            return;
        }
        f += 6;
        while (*f == ' ' || *f == '"') f++;
        char cmd[256];
        int ci = 0;
        while (*f && *f != '"' && ci < 255) cmd[ci++] = *f++;
        cmd[ci] = '\0';

        /* Send OK first, then dispatch */
        mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");

        char cmd_copy[256];
        snprintf(cmd_copy, sizeof(cmd_copy), "%s", cmd);
        const char *argv[16];
        int argc = 0;
        char *tok = strtok(cmd_copy, " ");
        while (tok && argc < 16) {
            argv[argc++] = tok;
            tok = strtok(NULL, " ");
        }
        if (argc > 0) {
            kerchunk_resp_t resp;
            resp_init(&resp);
            kerchunk_dispatch_command(argc, argv, &resp);
            resp_finish(&resp);
        }
        return;
    }

    /* Send OK first, then dispatch */
    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");

    char cmd_copy[256];
    snprintf(cmd_copy, sizeof(cmd_copy), "%s", p);
    free((void *)p);
    const char *argv[16];
    int argc = 0;
    char *tok = strtok(cmd_copy, " ");
    while (tok && argc < 16) {
        argv[argc++] = tok;
        tok = strtok(NULL, " ");
    }
    if (argc > 0) {
        kerchunk_resp_t resp;
        resp_init(&resp);
        kerchunk_dispatch_command(argc, argv, &resp);
        resp_finish(&resp);
    }
}

/* ── Users API ── */

static void handle_api_users(struct mg_connection *c)
{
    char buf[RESP_MAX] = "{\"users\":[";
    int off = (int)strlen(buf);
    int count = kerchunk_user_count();
    for (int i = 0; i < count && off < RESP_MAX - 200; i++) {
        const kerchunk_user_t *u = kerchunk_user_get(i);
        if (!u) continue;
        if (i > 0) buf[off++] = ',';
        off += snprintf(buf + off, RESP_MAX - off,
            "{\"id\":%d,\"name\":\"%s\","
            "\"dtmf_login\":\"%s\",\"ani\":\"%s\",\"access\":%d,"
            "\"voicemail\":%d,\"group\":%d,\"tx_ctcss\":%d,\"tx_dcs\":%d,"
            "\"totp_secret\":\"%s\"}",
            u->id, u->name,
            u->dtmf_login, u->ani, u->access,
            u->voicemail, u->group,
            u->tx_ctcss_freq_x10, u->tx_dcs_code,
            u->totp_secret);
    }
    off += snprintf(buf + off, RESP_MAX - off, "]}");
    (void)off;
    mg_http_reply(c, 200, API_HEADERS, "%s", buf);
}

/* ── Groups API ── */

static void handle_api_groups(struct mg_connection *c)
{
    char buf[RESP_MAX] = "{\"groups\":[";
    int off = (int)strlen(buf);
    int count = kerchunk_group_count();
    for (int i = 0; i < count && off < RESP_MAX - 200; i++) {
        const kerchunk_group_t *g = kerchunk_group_get(i);
        if (!g) continue;
        if (i > 0) buf[off++] = ',';
        off += snprintf(buf + off, RESP_MAX - off,
            "{\"id\":%d,\"name\":\"%s\",\"tx_ctcss\":%d,\"tx_dcs\":%d}",
            g->id, g->name, g->tx_ctcss_freq_x10, g->tx_dcs_code);
    }
    off += snprintf(buf + off, RESP_MAX - off, "]}");
    (void)off;
    mg_http_reply(c, 200, API_HEADERS, "%s", buf);
}

/* ── URI id helper ── */

static int uri_id(struct mg_http_message *hm, const char *prefix)
{
    if (hm->uri.len <= strlen(prefix)) return -1;
    return atoi(hm->uri.buf + strlen(prefix));
}

/* ── User CRUD ── */

static void handle_api_user_create(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    char *name = mg_json_get_str(hm->body, "$.name");
    if (!name) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Missing name\"}");
        return;
    }

    kerchunk_core_lock_config();

    /* Find next available user ID */
    int new_id = -1;
    for (int id = 1; id <= KERCHUNK_MAX_USERS; id++) {
        if (!kerchunk_user_lookup_by_id(id)) { new_id = id; break; }
    }
    if (new_id < 0) {
        kerchunk_core_unlock_config();
        free(name);
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Max users reached\"}");
        return;
    }

    char section[32], vbuf[256];
    snprintf(section, sizeof(section), "user.%d", new_id);
    kerchunk_config_set(cfg, section, "name", name);
    free(name);

    char *s;
    long lv;

    s = mg_json_get_str(hm->body, "$.dtmf_login");
    kerchunk_config_set(cfg, section, "dtmf_login", s ? s : "");
    free(s);

    s = mg_json_get_str(hm->body, "$.ani");
    kerchunk_config_set(cfg, section, "ani", s ? s : "");
    free(s);

    lv = mg_json_get_long(hm->body, "$.access", 1);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "access", vbuf);

    lv = mg_json_get_long(hm->body, "$.voicemail", 0);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "voicemail", vbuf);

    lv = mg_json_get_long(hm->body, "$.group", 0);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "group", vbuf);

    lv = mg_json_get_long(hm->body, "$.tx_ctcss", 0);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "tx_ctcss", vbuf);

    lv = mg_json_get_long(hm->body, "$.tx_dcs", 0);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "tx_dcs", vbuf);

    s = mg_json_get_str(hm->body, "$.totp_secret");
    kerchunk_config_set(cfg, section, "totp_secret", s ? s : "");
    free(s);

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true,\"id\":%d}", new_id);
}

static void handle_api_user_update(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    int id = uri_id(hm, "/api/users/");
    if (id < 1 || id > KERCHUNK_MAX_USERS) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Invalid user ID\"}");
        return;
    }

    kerchunk_core_lock_config();

    char section[32], vbuf[256];
    snprintf(section, sizeof(section), "user.%d", id);

    char *s;
    long lv;

    s = mg_json_get_str(hm->body, "$.name");
    if (s) { kerchunk_config_set(cfg, section, "name", s); free(s); }

    s = mg_json_get_str(hm->body, "$.dtmf_login");
    if (s) { kerchunk_config_set(cfg, section, "dtmf_login", s); free(s); }

    s = mg_json_get_str(hm->body, "$.ani");
    if (s) { kerchunk_config_set(cfg, section, "ani", s); free(s); }

    lv = mg_json_get_long(hm->body, "$.access", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "access", vbuf); }

    lv = mg_json_get_long(hm->body, "$.voicemail", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "voicemail", vbuf); }

    lv = mg_json_get_long(hm->body, "$.group", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "group", vbuf); }

    lv = mg_json_get_long(hm->body, "$.tx_ctcss", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "tx_ctcss", vbuf); }

    lv = mg_json_get_long(hm->body, "$.tx_dcs", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "tx_dcs", vbuf); }

    s = mg_json_get_str(hm->body, "$.totp_secret");
    if (s) { kerchunk_config_set(cfg, section, "totp_secret", s); free(s); }

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

static void handle_api_user_delete(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    int id = uri_id(hm, "/api/users/");
    if (id < 1 || id > KERCHUNK_MAX_USERS) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Invalid user ID\"}");
        return;
    }

    kerchunk_core_lock_config();

    char section[32];
    snprintf(section, sizeof(section), "user.%d", id);
    kerchunk_config_remove_section(cfg, section);
    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

/* ── Group CRUD ── */

static void handle_api_group_create(struct mg_connection *c,
                                     struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    char *name = mg_json_get_str(hm->body, "$.name");
    if (!name) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Missing name\"}");
        return;
    }

    kerchunk_core_lock_config();

    /* Find next available group ID */
    int new_id = -1;
    for (int id = 1; id <= KERCHUNK_MAX_GROUPS; id++) {
        if (!kerchunk_group_lookup_by_id(id)) { new_id = id; break; }
    }
    if (new_id < 0) {
        kerchunk_core_unlock_config();
        free(name);
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Max groups reached\"}");
        return;
    }

    char section[32], vbuf[256];
    snprintf(section, sizeof(section), "group.%d", new_id);
    kerchunk_config_set(cfg, section, "name", name);
    free(name);

    long lv;

    lv = mg_json_get_long(hm->body, "$.tx_ctcss", 0);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "tx_ctcss", vbuf);

    lv = mg_json_get_long(hm->body, "$.tx_dcs", 0);
    snprintf(vbuf, sizeof(vbuf), "%ld", lv);
    kerchunk_config_set(cfg, section, "tx_dcs", vbuf);

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true,\"id\":%d}", new_id);
}

static void handle_api_group_update(struct mg_connection *c,
                                     struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    int id = uri_id(hm, "/api/groups/");
    if (id < 1 || id > KERCHUNK_MAX_GROUPS) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Invalid group ID\"}");
        return;
    }

    kerchunk_core_lock_config();

    char section[32], vbuf[256];
    snprintf(section, sizeof(section), "group.%d", id);

    char *s;
    long lv;

    s = mg_json_get_str(hm->body, "$.name");
    if (s) { kerchunk_config_set(cfg, section, "name", s); free(s); }

    lv = mg_json_get_long(hm->body, "$.tx_ctcss", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "tx_ctcss", vbuf); }

    lv = mg_json_get_long(hm->body, "$.tx_dcs", -1);
    if (lv >= 0) { snprintf(vbuf, sizeof(vbuf), "%ld", lv);
                    kerchunk_config_set(cfg, section, "tx_dcs", vbuf); }

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

static void handle_api_group_delete(struct mg_connection *c,
                                     struct mg_http_message *hm)
{
    kerchunk_config_t *cfg = kerchunk_core_get_users_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    int id = uri_id(hm, "/api/groups/");
    if (id < 1 || id > KERCHUNK_MAX_GROUPS) {
        mg_http_reply(c, 400, API_HEADERS, "{\"error\":\"Invalid group ID\"}");
        return;
    }

    kerchunk_core_lock_config();

    char section[32];
    snprintf(section, sizeof(section), "group.%d", id);
    kerchunk_config_remove_section(cfg, section);
    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

/* ── Config API ── */

static void handle_api_config_get(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    (void)hm;
    if (g_auth_token[0] == '\0') {
        mg_http_reply(c, 403, API_HEADERS,
                      "{\"error\":\"auth_token not configured\"}");
        return;
    }

    kerchunk_config_t *cfg = kerchunk_core_get_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    kerchunk_core_lock_config();

    char buf[16384] = "{\"sections\":{";
    int off = (int)strlen(buf);
    int sec_iter = 0;
    const char *section;
    int first_sec = 1;

    while ((section = kerchunk_config_next_section(cfg, &sec_iter)) != NULL) {
        if (strncmp(section, "user.", 5) == 0 ||
            strncmp(section, "group.", 6) == 0)
            continue;

        if (!first_sec && off < (int)sizeof(buf) - 2)
            buf[off++] = ',';
        first_sec = 0;

        off += snprintf(buf + off, sizeof(buf) - off, "\"%s\":{", section);

        int key_iter = 0;
        const char *key, *val;
        int first_key = 1;
        while ((key = kerchunk_config_next_key(cfg, section, &key_iter,
                                                &val)) != NULL) {
            if (!first_key && off < (int)sizeof(buf) - 2)
                buf[off++] = ',';
            first_key = 0;

            const char *show = is_sensitive(key) ? "********" : val;
            off += snprintf(buf + off, sizeof(buf) - off,
                            "\"%s\":\"%s\"", key, show);

            if (off >= (int)sizeof(buf) - 64) break;
        }

        off += snprintf(buf + off, sizeof(buf) - off, "}");
        if (off >= (int)sizeof(buf) - 64) break;
    }

    off += snprintf(buf + off, sizeof(buf) - off, "}}");
    (void)off;
    kerchunk_core_unlock_config();
    mg_http_reply(c, 200, API_HEADERS, "%s", buf);
}

static void handle_api_config_put(struct mg_connection *c,
                                    struct mg_http_message *hm)
{
    if (g_auth_token[0] == '\0') {
        mg_http_reply(c, 403, API_HEADERS,
                      "{\"error\":\"auth_token not configured\"}");
        return;
    }

    kerchunk_config_t *cfg = kerchunk_core_get_config();
    if (!cfg) {
        mg_http_reply(c, 500, API_HEADERS, "{\"error\":\"No config\"}");
        return;
    }

    char *section = mg_json_get_str(hm->body, "$.section");
    if (!section) {
        mg_http_reply(c, 400, API_HEADERS,
                      "{\"error\":\"Missing section\"}");
        return;
    }

    if (strncmp(section, "user.", 5) == 0 ||
        strncmp(section, "group.", 6) == 0) {
        free(section);
        mg_http_reply(c, 403, API_HEADERS,
                      "{\"error\":\"Use users/groups API\"}");
        return;
    }

    /* Find "values" object in body and iterate key:value pairs */
    const char *vstart = strstr(hm->body.buf, "\"values\"");
    if (!vstart) {
        free(section);
        mg_http_reply(c, 400, API_HEADERS,
                      "{\"error\":\"Missing values\"}");
        return;
    }

    kerchunk_core_lock_config();
    vstart = strchr(vstart, '{');
    if (vstart) {
        vstart++; /* skip { */
        char key[64], val[256];
        while (*vstart && *vstart != '}') {
            while (*vstart && (*vstart == ' ' || *vstart == ',' ||
                               *vstart == '\n' || *vstart == '\r' ||
                               *vstart == '\t'))
                vstart++;
            if (*vstart == '"') {
                vstart++;
                int ki = 0;
                while (*vstart && *vstart != '"' && ki < 63)
                    key[ki++] = *vstart++;
                key[ki] = '\0';
                if (*vstart == '"') vstart++;
                while (*vstart && *vstart != ':') vstart++;
                if (*vstart == ':') vstart++;
                while (*vstart == ' ') vstart++;
                if (*vstart == '"') {
                    vstart++;
                    int vi = 0;
                    while (*vstart && *vstart != '"' && vi < 255)
                        val[vi++] = *vstart++;
                    val[vi] = '\0';
                    if (*vstart == '"') vstart++;

                    if (is_sensitive(key) && strcmp(val, "********") == 0)
                        continue;
                    kerchunk_config_set(cfg, section, key, val);
                }
            } else {
                break;
            }
        }
    }

    kerchunk_config_save(cfg);
    kerchunk_core_unlock_config();
    kill(getpid(), SIGHUP);
    free(section);

    mg_http_reply(c, 200, API_HEADERS, "{\"ok\":true}");
}

/* ── Mongoose event handler ── */

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev == MG_EV_ACCEPT && g_tls_active) {
        struct mg_tls_opts opts = { .cert = g_cert_data, .key = g_key_data };
        mg_tls_init(c, &opts);
    }

    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message *hm = (struct mg_http_message *)ev_data;

        /* CORS preflight */
        if (mg_match(hm->method, mg_str("OPTIONS"), NULL)) {
            mg_http_reply(c, 204, CORS_HEADERS, "");
            return;
        }

        /* API routes */
        if (mg_match(hm->uri, mg_str("/api/#"), NULL)) {
            if (!check_auth(hm)) {
                mg_http_reply(c, 401, API_HEADERS,
                              "{\"error\":\"Invalid or missing token\"}");
                return;
            }

            /* SSE */
            if (mg_match(hm->uri, mg_str("/api/events"), NULL)) {
                mg_printf(c,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/event-stream\r\n"
                    "Cache-Control: no-cache\r\n"
                    "Connection: keep-alive\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "\r\n");
                c->data[0] = 'E';  /* Mark as SSE client */
                atomic_fetch_add(&g_sse_count, 1);
                return;
            }

            /* POST /api/config/reload */
            if (mg_match(hm->method, mg_str("POST"), NULL) &&
                mg_match(hm->uri, mg_str("/api/config/reload"), NULL)) {
                kill(getpid(), SIGHUP);
                mg_http_reply(c, 200, API_HEADERS,
                              "{\"ok\":true,\"action\":\"config_reload\"}");
                return;
            }

            /* Config CRUD (GET/PUT /api/config) */
            if (mg_match(hm->uri, mg_str("/api/config"), NULL)) {
                if (mg_match(hm->method, mg_str("GET"), NULL))
                    handle_api_config_get(c, hm);
                else if (mg_match(hm->method, mg_str("PUT"), NULL))
                    handle_api_config_put(c, hm);
                else
                    mg_http_reply(c, 405, API_HEADERS,
                                  "{\"error\":\"Method not allowed\"}");
                return;
            }

            /* POST /api/cmd */
            if (mg_match(hm->method, mg_str("POST"), NULL) &&
                mg_match(hm->uri, mg_str("/api/cmd"), NULL)) {
                handle_api_post_cmd(c, hm);
                return;
            }

            /* Users CRUD */
            if (mg_match(hm->method, mg_str("POST"), NULL) &&
                mg_match(hm->uri, mg_str("/api/users"), NULL)) {
                handle_api_user_create(c, hm);
                return;
            }
            if (mg_match(hm->uri, mg_str("/api/users/#"), NULL)) {
                if (mg_match(hm->method, mg_str("PUT"), NULL))
                    handle_api_user_update(c, hm);
                else if (mg_match(hm->method, mg_str("DELETE"), NULL))
                    handle_api_user_delete(c, hm);
                else
                    mg_http_reply(c, 405, API_HEADERS, "{\"error\":\"Method not allowed\"}");
                return;
            }

            /* Groups CRUD */
            if (mg_match(hm->method, mg_str("POST"), NULL) &&
                mg_match(hm->uri, mg_str("/api/groups"), NULL)) {
                handle_api_group_create(c, hm);
                return;
            }
            if (mg_match(hm->uri, mg_str("/api/groups/#"), NULL)) {
                if (mg_match(hm->method, mg_str("PUT"), NULL))
                    handle_api_group_update(c, hm);
                else if (mg_match(hm->method, mg_str("DELETE"), NULL))
                    handle_api_group_delete(c, hm);
                else
                    mg_http_reply(c, 405, API_HEADERS, "{\"error\":\"Method not allowed\"}");
                return;
            }
            if (mg_match(hm->uri, mg_str("/api/groups"), NULL)) {
                handle_api_groups(c);
                return;
            }

            /* GET /api/users */
            if (mg_match(hm->uri, mg_str("/api/users"), NULL)) {
                handle_api_users(c);
                return;
            }

            /* GET /api route table */
            handle_api_get(c, hm);
            return;
        }

        /* Static files */
        if (g_static_dir[0]) {
            struct mg_http_serve_opts opts = { .root_dir = g_static_dir };
            mg_http_serve_dir(c, hm, &opts);
        } else {
            mg_http_reply(c, 404, "", "Not found\n");
        }
    }

    /* SSE broadcast received from another thread via mg_wakeup() */
    if (ev == MG_EV_WAKEUP) {
        struct mg_str *data = (struct mg_str *)ev_data;
        for (struct mg_connection *t = c->mgr->conns; t != NULL; t = t->next) {
            if (t->data[0] == 'E')
                mg_printf(t, "data: %.*s\n\n", (int)data->len, data->buf);
        }
    }

    /* Track SSE client disconnections */
    if (ev == MG_EV_CLOSE && c->data[0] == 'E') {
        atomic_fetch_sub(&g_sse_count, 1);
    }
}

/* ── Event handler for SSE broadcast ── */

static void web_event_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (evt->type == KERCHEVT_AUDIO_FRAME || evt->type == KERCHEVT_TICK ||
        evt->type == KERCHEVT_CTCSS_DETECT || evt->type == KERCHEVT_DCS_DETECT)
        return;
    if (atomic_load(&g_sse_count) <= 0)
        return;

    char json[512];
    int jlen = kerchevt_to_json(evt, json, sizeof(json));
    if (jlen > 0)
        mg_wakeup(&g_mgr, g_listener_id, json, (size_t)jlen);
}

/* ── Server thread ── */

static void *web_thread(void *arg)
{
    (void)arg;
    while (g_running)
        mg_mgr_poll(&g_mgr, 20);  /* 20ms poll */
    return NULL;
}

/* ── Module lifecycle ── */

static int web_load(kerchunk_core_t *core)
{
    g_core = core;
    return 0;
}

static int web_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "web", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    g_port = kerchunk_config_get_int(cfg, "web", "port", 8080);

    v = kerchunk_config_get(cfg, "web", "bind");
    if (v) snprintf(g_bind, sizeof(g_bind), "%s", v);

    v = kerchunk_config_get(cfg, "web", "auth_token");
    if (v) snprintf(g_auth_token, sizeof(g_auth_token), "%s", v);

    v = kerchunk_config_get(cfg, "web", "static_dir");
    if (v) snprintf(g_static_dir, sizeof(g_static_dir), "%s", v);

    v = kerchunk_config_get(cfg, "web", "tls_cert");
    if (v) snprintf(g_tls_cert, sizeof(g_tls_cert), "%s", v);

    v = kerchunk_config_get(cfg, "web", "tls_key");
    if (v) snprintf(g_tls_key, sizeof(g_tls_key), "%s", v);

    /* Redirect mongoose logs through our logger */
    mg_log_set_fn(mg_log_cb, NULL);
    mg_log_set(MG_LL_DEBUG);

    /* If already running, just update config values (auth_token, static_dir)
     * without restarting — restarting would deadlock if triggered by a web
     * API handler on the mongoose thread via SIGHUP. */
    if (g_running) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "config reloaded (port=%d auth=%s)",
                    g_port, g_auth_token[0] ? "yes" : "no");
        return 0;
    }

    if (!g_enabled) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled");
        return 0;
    }

    if (g_cert_data.buf) { free(g_cert_data.buf); g_cert_data.buf = NULL; }
    if (g_key_data.buf) { free(g_key_data.buf); g_key_data.buf = NULL; }

    /* TLS setup — read cert/key files once */
    g_tls_active = 0;
    if (g_tls_cert[0] && g_tls_key[0]) {
        g_cert_data = mg_file_read(&mg_fs_posix, g_tls_cert);
        if (g_cert_data.buf == NULL) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "failed to read TLS cert: %s", g_tls_cert);
            g_enabled = 0;
            return 0;
        }
        g_key_data = mg_file_read(&mg_fs_posix, g_tls_key);
        if (g_key_data.buf == NULL) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "failed to read TLS key: %s", g_tls_key);
            free(g_cert_data.buf);
            g_cert_data.buf = NULL;
            g_enabled = 0;
            return 0;
        }
        g_tls_active = 1;
    }

    /* Initialize mongoose */
    mg_mgr_init(&g_mgr);
    mg_wakeup_init(&g_mgr);

    /* Make wakeup pipe non-blocking so event producers (audio thread,
     * main thread) never block if the pipe buffer is full. Mongoose's
     * MSG_NONBLOCKING is defined as 0 on Linux, so send() would block. */
    if (g_mgr.pipe != MG_INVALID_SOCKET)
        fcntl((int)(size_t)g_mgr.pipe, F_SETFL,
              fcntl((int)(size_t)g_mgr.pipe, F_GETFL) | O_NONBLOCK);

    char url[128];
    snprintf(url, sizeof(url), "%s://%s:%d",
             g_tls_active ? "https" : "http", g_bind, g_port);

    struct mg_connection *listener = mg_http_listen(&g_mgr, url,
                                                     ev_handler, NULL);
    if (!listener) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "failed to listen on %s", url);
        mg_mgr_free(&g_mgr);
        if (g_cert_data.buf) { free(g_cert_data.buf); g_cert_data.buf = NULL; }
        if (g_key_data.buf) { free(g_key_data.buf); g_key_data.buf = NULL; }
        g_enabled = 0;
        return 0;
    }
    g_listener_id = listener->id;

    /* Subscribe to events for SSE broadcast (unsubscribe first to avoid
     * duplicates on config reload — configure() is called on every SIGHUP) */
    static const kerchevt_type_t types[] = {
        KERCHEVT_COR_ASSERT, KERCHEVT_COR_DROP, KERCHEVT_PTT_ASSERT, KERCHEVT_PTT_DROP,
        KERCHEVT_STATE_CHANGE, KERCHEVT_TAIL_START, KERCHEVT_TAIL_EXPIRE, KERCHEVT_TIMEOUT,
        KERCHEVT_CALLER_IDENTIFIED, KERCHEVT_CALLER_CLEARED,
        KERCHEVT_DTMF_DIGIT, KERCHEVT_DTMF_END,
        KERCHEVT_QUEUE_DRAIN, KERCHEVT_QUEUE_COMPLETE, KERCHEVT_RECORDING_SAVED,
        KERCHEVT_CONFIG_RELOAD, KERCHEVT_SHUTDOWN,
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        g_core->unsubscribe(types[i], web_event_handler);
    for (int i = 0; i <= 15; i++)
        g_core->unsubscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i), web_event_handler);

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        g_core->subscribe(types[i], web_event_handler, NULL);
    for (int i = 0; i <= 15; i++)
        g_core->subscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i),
                           web_event_handler, NULL);

    /* Start server thread */
    g_running = 1;
    g_sse_count = 0;
    if (pthread_create(&g_web_thread, NULL, web_thread, NULL) != 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create web thread");
        mg_mgr_free(&g_mgr);
        g_enabled = 0;
        return 0;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "listening on %s://%s:%d%s",
                g_tls_active ? "https" : "http",
                g_bind, g_port,
                g_auth_token[0] ? " (auth required)" : "");
    return 0;
}

static void web_unload(void)
{
    /* Unsubscribe from all event types */
    static const kerchevt_type_t types[] = {
        KERCHEVT_COR_ASSERT, KERCHEVT_COR_DROP, KERCHEVT_PTT_ASSERT, KERCHEVT_PTT_DROP,
        KERCHEVT_STATE_CHANGE, KERCHEVT_TAIL_START, KERCHEVT_TAIL_EXPIRE, KERCHEVT_TIMEOUT,
        KERCHEVT_CALLER_IDENTIFIED, KERCHEVT_CALLER_CLEARED,
        KERCHEVT_DTMF_DIGIT, KERCHEVT_DTMF_END,
        KERCHEVT_QUEUE_DRAIN, KERCHEVT_QUEUE_COMPLETE, KERCHEVT_RECORDING_SAVED,
        KERCHEVT_CONFIG_RELOAD, KERCHEVT_SHUTDOWN,
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        g_core->unsubscribe(types[i], web_event_handler);
    for (int i = 0; i <= 15; i++)
        g_core->unsubscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + i), web_event_handler);

    if (g_running) {
        g_running = 0;
        pthread_join(g_web_thread, NULL);
    }

    mg_mgr_free(&g_mgr);

    if (g_cert_data.buf) { free(g_cert_data.buf); g_cert_data.buf = NULL; }
    if (g_key_data.buf) { free(g_key_data.buf); g_key_data.buf = NULL; }
}

/* ── CLI ── */

static int cli_web(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    resp_bool(r, "enabled", g_enabled);
    resp_int(r, "port", g_port);
    resp_str(r, "bind", g_bind);
    resp_bool(r, "auth", g_auth_token[0] != '\0');
    resp_bool(r, "tls", g_tls_active);
    if (g_tls_active)
        resp_str(r, "tls_cert", g_tls_cert);
    resp_int(r, "sse_clients", atomic_load(&g_sse_count));
    if (g_static_dir[0])
        resp_str(r, "static_dir", g_static_dir);
    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "web", "web", "Web server status", cli_web },
};

static kerchunk_module_def_t mod_web = {
    .name             = "mod_web",
    .version          = "2.0.0",
    .description      = "Embedded HTTP/HTTPS server for web dashboard",
    .load             = web_load,
    .configure        = web_configure,
    .unload           = web_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_web);
