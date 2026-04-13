/*
 * kerchunk_socket.c — Unix domain socket server for CLI control
 *
 * Persistent-connection framed protocol:
 *   R <text>    Response line (part of a command reply)
 *   .           End-of-response sentinel (". text" for welcome)
 *   E <text>    Async event/log line (server-pushed)
 *
 * Meta-commands (never collide with real commands):
 *   __COMMANDS__            Return all unique command names (tab-separated)
 *   __COMPLETIONS__         Return per-subcommand completion table
 *   __USERS__               Return newline-separated usernames (for tab completion)
 *   __MODULES__             Return newline-separated loaded module names
 *   __SOUNDS__              Return newline-separated <subdir>/<name> sound paths
 *   __SUBSCRIBE__ <level>   Start log streaming (3=error, 4=warn, 6=info, 7=debug)
 *   __UNSUBSCRIBE__         Stop log streaming
 *
 * Socket restricted to owner (chmod 0600) with peer UID verification.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_user.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#define LOG_MOD       "socket"
#define MAX_CMD_LEN   1024
#define MAX_RESP_LEN  4096
#define MAX_CLIENTS   8
#define EVT_RING_SIZE 64
/* Log broadcast buffer. Must be larger than the max formatted log
 * line (1024-byte msg + 32-byte timestamp + ~20 bytes of format
 * overhead) or CLI subscribers will see truncated lines — notably
 * the TTS normalizer's "TN: ... -> ..." output which can run 600+
 * chars after normalization. */
#define EVT_LINE_MAX  1152

/* Core commands — uses kerchunk_cli_cmd_t directly (from kerchunk_module.h) */

/* Per-client connection state */
typedef struct {
    int   fd;
    int   subscribed;        /* 1 = receiving log stream */
    int   subscribe_level;   /* min log level to forward */
    int   in_response;       /* 1 = writing R lines (defer events) */
    int   json_mode;         /* 1 = send JSON responses for this command */
    int   json_events;      /* 1 = send structured JSON events (not log text) */
    char  cmd_buf[MAX_CMD_LEN];
    int   cmd_len;           /* accumulated partial-command bytes */
    char  evt_ring[EVT_RING_SIZE][EVT_LINE_MAX];
    int   evt_head;
    int   evt_tail;
} client_slot_t;

static int             g_listen_fd = -1;
static client_slot_t   g_clients[MAX_CLIENTS];
static pthread_mutex_t g_client_mutex = PTHREAD_MUTEX_INITIALIZER;
static char            g_socket_path[108] = "/run/kerchunk/kerchunk.sock";

static const kerchunk_cli_cmd_t *g_core_cmds;
static int                       g_num_core_cmds;

/* Sounds directory for __SOUNDS__ enumeration. Set at startup from config. */
static char g_sounds_dir[256] = "";

/* ── Helpers ───────────────────────────────────────────────────────── */

static void init_slot(client_slot_t *c)
{
    c->fd              = -1;
    c->subscribed      = 0;
    c->subscribe_level = KERCHUNK_LOG_INFO;
    c->in_response     = 0;
    c->json_mode       = 0;
    c->json_events     = 0;
    c->cmd_len         = 0;
    c->evt_head        = 0;
    c->evt_tail        = 0;
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) break;
        buf += n;
        len -= (size_t)n;
    }
}

static int check_peer_uid(int fd)
{
#ifdef __linux__
    struct ucred cred;
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
        return -1;
    if (cred.uid != getuid() && cred.uid != 0) {
        KERCHUNK_LOG_W(LOG_MOD, "rejected connection from uid %d", cred.uid);
        return -1;
    }
#elif defined(__APPLE__) || defined(__FreeBSD__)
    uid_t euid;
    gid_t egid;
    extern int getpeereid(int, uid_t *, gid_t *);
    if (getpeereid(fd, &euid, &egid) < 0)
        return -1;
    if (euid != getuid() && euid != 0) {
        KERCHUNK_LOG_W(LOG_MOD, "rejected connection from uid %d", euid);
        return -1;
    }
#endif
    return 0;
}

/* ── Response framing ──────────────────────────────────────────────── */

static void begin_response(client_slot_t *c)
{
    pthread_mutex_lock(&g_client_mutex);
    c->in_response = 1;
    pthread_mutex_unlock(&g_client_mutex);
}

/* Write ".\n" terminator, atomically clear in_response, flush deferred events */
static void end_response(client_slot_t *c)
{
    write_all(c->fd, ".\n", 2);

    pthread_mutex_lock(&g_client_mutex);
    c->in_response = 0;
    while (c->evt_head != c->evt_tail) {
        char line[EVT_LINE_MAX + 4];
        int n = snprintf(line, sizeof(line), "E %s\n",
                         c->evt_ring[c->evt_head]);
        c->evt_head = (c->evt_head + 1) % EVT_RING_SIZE;
        write_all(c->fd, line, (size_t)n);
    }
    pthread_mutex_unlock(&g_client_mutex);
}

/* Send text as framed R-lines, then end-of-response */
static void send_framed_response(client_slot_t *c, const char *text)
{
    begin_response(c);

    if (text && text[0]) {
        const char *p = text;
        while (*p) {
            const char *eol = strchr(p, '\n');
            if (!eol) eol = p + strlen(p);
            if (eol > p) {
                char line[MAX_RESP_LEN + 4];
                int len = snprintf(line, sizeof(line), "R %.*s\n",
                                   (int)(eol - p), p);
                write_all(c->fd, line, (size_t)len);
            }
            p = (*eol == '\n') ? eol + 1 : eol;
        }
    }

    end_response(c);
}

/* ── Command parsing ───────────────────────────────────────────────── */

static int parse_cmd(char *line, const char **argv, int max_argv)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max_argv) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n')
            break;
        if (*p == '"') {
            p++;  /* skip opening quote */
            argv[argc++] = p;
            while (*p && *p != '"')
                p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\n')
                p++;
        }
        if (*p)
            *p++ = '\0';
    }
    return argc;
}

/* ── Meta-commands ─────────────────────────────────────────────────── */

/* Dedup state for __COMMANDS__ — multiple cli_cmd_t entries can share a
 * name (e.g. mod_pocsag has three entries for send/numeric/tone forms).
 * Completion only wants each name once. */
#define MAX_SEEN_NAMES 256
typedef struct {
    client_slot_t *c;
    const char *seen[MAX_SEEN_NAMES];
    int seen_count;
} cmd_dedup_ctx_t;

static int dedup_seen(cmd_dedup_ctx_t *ctx, const char *name)
{
    for (int i = 0; i < ctx->seen_count; i++) {
        if (strcmp(ctx->seen[i], name) == 0)
            return 1;
    }
    if (ctx->seen_count < MAX_SEEN_NAMES)
        ctx->seen[ctx->seen_count++] = name;
    return 0;
}

static void emit_command_row(client_slot_t *c, const kerchunk_cli_cmd_t *cmd)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "R %s\t%s\t%s\n",
                     cmd->name,
                     cmd->usage ? cmd->usage : cmd->name,
                     cmd->description ? cmd->description : "");
    write_all(c->fd, line, (size_t)n);
}

static void cmd_iter_cb(const kerchunk_cli_cmd_t *cmd, void *ud)
{
    cmd_dedup_ctx_t *ctx = (cmd_dedup_ctx_t *)ud;
    if (dedup_seen(ctx, cmd->name))
        return;
    emit_command_row(ctx->c, cmd);
}

static void handle_meta_commands(client_slot_t *c)
{
    begin_response(c);

    cmd_dedup_ctx_t ctx = { .c = c, .seen_count = 0 };

    for (int i = 0; i < g_num_core_cmds; i++) {
        if (dedup_seen(&ctx, g_core_cmds[i].name))
            continue;
        emit_command_row(c, &g_core_cmds[i]);
    }

    kerchunk_module_iter_cli_commands(cmd_iter_cb, &ctx);
    end_response(c);
}

/* __COMPLETIONS__ — return one row per cli_cmd_t entry with structured
 * subcommand + field info for tab completion.
 *
 * Row format:
 *   R <command>\t<subcommand>\t<field1>\t<field2>...
 * where each <field> is "<name>:<type>:<options>" (options may be empty).
 *
 * <subcommand> is the second token of ui_command (e.g. "pocsag send" → "send").
 * If ui_command is NULL or has no second token, <subcommand> is empty.
 *
 * subcommands field (P2): comma-separated list of subcommand names emitted
 * one row per name with no fields, so first-word→subcommand completion works
 * even when no ui form is wired up.
 */
static void emit_completion_row(client_slot_t *c, const kerchunk_cli_cmd_t *cmd)
{
    char line[1024];
    size_t off = 0;

    /* Extract subcommand from ui_command: "pocsag send" → "send" */
    const char *sub = "";
    char sub_buf[64];
    if (cmd->ui_command) {
        const char *space = strchr(cmd->ui_command, ' ');
        if (space) {
            const char *s = space + 1;
            const char *end = strchr(s, ' ');
            size_t len = end ? (size_t)(end - s) : strlen(s);
            if (len >= sizeof(sub_buf)) len = sizeof(sub_buf) - 1;
            memcpy(sub_buf, s, len);
            sub_buf[len] = '\0';
            sub = sub_buf;
        }
    }

    int n = snprintf(line + off, sizeof(line) - off, "R %s\t%s",
                     cmd->name, sub);
    if (n < 0 || (size_t)n >= sizeof(line) - off) return;
    off += (size_t)n;

    for (int i = 0; i < cmd->num_ui_fields; i++) {
        const kerchunk_ui_field_t *f = &cmd->ui_fields[i];
        n = snprintf(line + off, sizeof(line) - off, "\t%s:%s:%s",
                     f->name ? f->name : "",
                     f->type ? f->type : "text",
                     f->options ? f->options : "");
        if (n < 0 || (size_t)n >= sizeof(line) - off) break;
        off += (size_t)n;
    }

    if (off + 2 < sizeof(line)) {
        line[off++] = '\n';
        line[off] = '\0';
    } else {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
        off = sizeof(line) - 1;
    }

    write_all(c->fd, line, off);
}

/* If cmd->subcommands is set (P2), emit one row per subcommand name with
 * empty field list. Skips entries that already have a ui_command (those are
 * emitted by emit_completion_row directly with their fields). */
static void emit_subcommand_rows(client_slot_t *c, const kerchunk_cli_cmd_t *cmd)
{
    if (!cmd->subcommands || !cmd->subcommands[0]) return;

    const char *p = cmd->subcommands;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' && *p != ' ') p++;
        size_t len = (size_t)(p - start);

        char line[256];
        int n = snprintf(line, sizeof(line), "R %s\t%.*s\n",
                         cmd->name, (int)len, start);
        if (n > 0) write_all(c->fd, line, (size_t)n);
    }
}

static void completions_iter_cb(const kerchunk_cli_cmd_t *cmd, void *ud)
{
    client_slot_t *c = (client_slot_t *)ud;
    emit_completion_row(c, cmd);
    emit_subcommand_rows(c, cmd);
}

static void handle_meta_completions(client_slot_t *c)
{
    begin_response(c);

    for (int i = 0; i < g_num_core_cmds; i++) {
        emit_completion_row(c, &g_core_cmds[i]);
        emit_subcommand_rows(c, &g_core_cmds[i]);
    }

    kerchunk_module_iter_cli_commands(completions_iter_cb, c);
    end_response(c);
}

/* __USERS__ — newline-separated usernames for tab completion */
static void handle_meta_users(client_slot_t *c)
{
    begin_response(c);
    int n = kerchunk_user_count();
    for (int i = 0; i < n; i++) {
        const kerchunk_user_t *u = kerchunk_user_get(i);
        if (!u || !u->username[0]) continue;
        char line[64];
        int len = snprintf(line, sizeof(line), "R %s\n", u->username);
        write_all(c->fd, line, (size_t)len);
    }
    end_response(c);
}

/* __MODULES__ — newline-separated loaded module names for tab completion */
static void handle_meta_modules(client_slot_t *c)
{
    begin_response(c);
    int n = kerchunk_module_count();
    for (int i = 0; i < n; i++) {
        const kerchunk_module_def_t *m = kerchunk_module_get(i);
        if (!m || !m->name) continue;
        /* Strip "mod_" prefix to match the names users type */
        const char *name = m->name;
        if (strncmp(name, "mod_", 4) == 0) name += 4;
        char line[64];
        int len = snprintf(line, sizeof(line), "R %s\n", name);
        write_all(c->fd, line, (size_t)len);
    }
    end_response(c);
}

/* __SOUNDS__ — walk one level into sounds_dir, emit <subdir>/<basename>
 * (without .wav extension) for tab completion. */
static void handle_meta_sounds(client_slot_t *c)
{
    begin_response(c);
    if (!g_sounds_dir[0]) { end_response(c); return; }

    DIR *d = opendir(g_sounds_dir);
    if (!d) { end_response(c); return; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char subpath[512];
        snprintf(subpath, sizeof(subpath), "%s/%s", g_sounds_dir, de->d_name);

        struct stat st;
        if (stat(subpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Walk one level deep */
            DIR *sd = opendir(subpath);
            if (!sd) continue;
            struct dirent *fe;
            while ((fe = readdir(sd)) != NULL) {
                if (fe->d_name[0] == '.') continue;
                /* Strip .wav suffix */
                size_t flen = strlen(fe->d_name);
                if (flen <= 4 || strcmp(fe->d_name + flen - 4, ".wav") != 0)
                    continue;
                char line[256];
                int len = snprintf(line, sizeof(line), "R %s/%.*s\n",
                                   de->d_name, (int)(flen - 4), fe->d_name);
                write_all(c->fd, line, (size_t)len);
            }
            closedir(sd);
        } else if (S_ISREG(st.st_mode)) {
            /* Top-level .wav file */
            size_t flen = strlen(de->d_name);
            if (flen <= 4 || strcmp(de->d_name + flen - 4, ".wav") != 0)
                continue;
            char line[256];
            int len = snprintf(line, sizeof(line), "R %.*s\n",
                               (int)(flen - 4), de->d_name);
            write_all(c->fd, line, (size_t)len);
        }
    }
    closedir(d);
    end_response(c);
}

static void handle_meta_subscribe(client_slot_t *c, const char *arg)
{
    int level = KERCHUNK_LOG_INFO;
    if (arg && *arg)
        level = atoi(arg);
    if (level < KERCHUNK_LOG_ERROR) level = KERCHUNK_LOG_ERROR;
    if (level > KERCHUNK_LOG_DEBUG) level = KERCHUNK_LOG_DEBUG;

    pthread_mutex_lock(&g_client_mutex);
    c->subscribed      = 1;
    c->subscribe_level = level;
    pthread_mutex_unlock(&g_client_mutex);

    write_all(c->fd, ".\n", 2);
}

static void handle_meta_unsubscribe(client_slot_t *c)
{
    pthread_mutex_lock(&g_client_mutex);
    c->subscribed = 0;
    pthread_mutex_unlock(&g_client_mutex);

    write_all(c->fd, ".\n", 2);
}

/* ── Command dispatch ──────────────────────────────────────────────── */

static void handle_command(client_slot_t *c, char *line)
{
    if (strncmp(line, "__COMMANDS__", 12) == 0) {
        handle_meta_commands(c);
        return;
    }
    if (strncmp(line, "__COMPLETIONS__", 15) == 0) {
        handle_meta_completions(c);
        return;
    }
    if (strncmp(line, "__USERS__", 9) == 0) {
        handle_meta_users(c);
        return;
    }
    if (strncmp(line, "__MODULES__", 11) == 0) {
        handle_meta_modules(c);
        return;
    }
    if (strncmp(line, "__SOUNDS__", 10) == 0) {
        handle_meta_sounds(c);
        return;
    }
    if (strncmp(line, "__SUBSCRIBE__", 13) == 0) {
        const char *arg = line + 13;
        while (*arg == ' ') arg++;
        handle_meta_subscribe(c, arg);
        return;
    }
    if (strncmp(line, "__UNSUBSCRIBE__", 15) == 0) {
        handle_meta_unsubscribe(c);
        return;
    }
    if (strncmp(line, "__JSON_EVENTS__", 15) == 0) {
        /* Subscribe to structured JSON events */
        pthread_mutex_lock(&g_client_mutex);
        c->subscribed      = 1;
        c->subscribe_level = KERCHUNK_LOG_DEBUG;
        c->json_events     = 1;
        pthread_mutex_unlock(&g_client_mutex);
        write_all(c->fd, ".\n", 2);
        return;
    }

    /* Detect and strip __JSON__ prefix */
    c->json_mode = 0;
    if (strncmp(line, "__JSON__ ", 9) == 0) {
        c->json_mode = 1;
        line += 9;
    }

    const char *argv[32];
    int argc = parse_cmd(line, argv, 32);
    if (argc == 0)
        return;

    kerchunk_resp_t resp;
    resp_init(&resp);
    kerchunk_dispatch_command(argc, argv, &resp);

    resp_finish(&resp);

    /* Send JSON or text based on client mode */
    const char *output = c->json_mode ? resp.json : resp.text;
    send_framed_response(c, output);
}

/* ── Log broadcast (called from any thread via kerchunk_log.c) ───────── */

void kerchunk_socket_broadcast_log(int level, const char *formatted_line)
{
    if (!formatted_line)
        return;

    pthread_mutex_lock(&g_client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_slot_t *c = &g_clients[i];
        if (c->fd < 0 || !c->subscribed || level > c->subscribe_level)
            continue;
        /* Skip text log for JSON event clients — they get structured events */
        if (c->json_events)
            continue;

        if (c->in_response) {
            int next = (c->evt_tail + 1) % EVT_RING_SIZE;
            if (next == c->evt_head)
                c->evt_head = (c->evt_head + 1) % EVT_RING_SIZE;
            snprintf(c->evt_ring[c->evt_tail], EVT_LINE_MAX,
                     "%s", formatted_line);
            c->evt_tail = next;
        } else {
            char line[EVT_LINE_MAX + 4];
            int n = snprintf(line, sizeof(line), "E %s\n", formatted_line);
            write_all(c->fd, line, (size_t)n);
        }
    }
    pthread_mutex_unlock(&g_client_mutex);
}

/* Broadcast structured event as JSON to __JSON_EVENTS__ subscribers.
 * Called from the event bus via a global handler registered at init. */
void kerchunk_socket_broadcast_event(const kerchevt_t *evt)
{
    if (!evt || evt->type == KERCHEVT_AUDIO_FRAME || evt->type == KERCHEVT_TICK)
        return;  /* Skip high-frequency events */

    char json[512];
    int jlen = kerchevt_to_json(evt, json, sizeof(json));
    if (jlen <= 0) return;

    char line[EVT_LINE_MAX + 4];
    int n = snprintf(line, sizeof(line), "E %s\n", json);

    pthread_mutex_lock(&g_client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_slot_t *c = &g_clients[i];
        if (c->fd < 0 || !c->json_events)
            continue;

        if (c->in_response) {
            int next = (c->evt_tail + 1) % EVT_RING_SIZE;
            if (next == c->evt_head)
                c->evt_head = (c->evt_head + 1) % EVT_RING_SIZE;
            snprintf(c->evt_ring[c->evt_tail], EVT_LINE_MAX, "%s", json);
            c->evt_tail = next;
        } else {
            write_all(c->fd, line, (size_t)n);
        }
    }
    pthread_mutex_unlock(&g_client_mutex);
}

/* ── Unified command dispatch (core + modules) ── */

int kerchunk_dispatch_command(int argc, const char **argv, kerchunk_resp_t *resp)
{
    if (argc == 0 || !argv || !resp) return -1;

    /* Try core commands first */
    if (g_core_cmds) {
        for (int i = 0; i < g_num_core_cmds; i++) {
            if (strcmp(argv[0], g_core_cmds[i].name) == 0) {
                g_core_cmds[i].handler(argc, argv, resp);
                return 0;
            }
        }
    }

    /* Try module commands */
    return kerchunk_module_dispatch_cli(argv[0], argc, argv, resp);
}

/* Event handler: forwards events to JSON event subscribers */
static void socket_event_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    kerchunk_socket_broadcast_event(evt);
}

/* ── Public API ────────────────────────────────────────────────────── */

int kerchunk_socket_init(const char *path)
{
    if (path)
        snprintf(g_socket_path, sizeof(g_socket_path), "%s", path);

    unlink(g_socket_path);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        KERCHUNK_LOG_E(LOG_MOD, "socket(): %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_socket_path);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        KERCHUNK_LOG_E(LOG_MOD, "bind %s: %s", g_socket_path, strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    chmod(g_socket_path, 0600);

    if (listen(g_listen_fd, 4) < 0) {
        KERCHUNK_LOG_E(LOG_MOD, "listen: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return -1;
    }

    if (fcntl(g_listen_fd, F_SETFL, O_NONBLOCK) < 0)
        KERCHUNK_LOG_W(LOG_MOD, "fcntl non-blocking failed: %s", strerror(errno));

    for (int i = 0; i < MAX_CLIENTS; i++)
        init_slot(&g_clients[i]);

    /* Subscribe to key event types for JSON event broadcasting */
    static const kerchevt_type_t evt_types[] = {
        KERCHEVT_COR_ASSERT, KERCHEVT_COR_DROP, KERCHEVT_VCOR_ASSERT, KERCHEVT_VCOR_DROP,
        KERCHEVT_PTT_ASSERT, KERCHEVT_PTT_DROP,
        KERCHEVT_STATE_CHANGE, KERCHEVT_TAIL_START, KERCHEVT_TAIL_EXPIRE, KERCHEVT_TIMEOUT,
        KERCHEVT_CALLER_IDENTIFIED, KERCHEVT_CALLER_CLEARED,
        KERCHEVT_CTCSS_DETECT, KERCHEVT_DCS_DETECT, KERCHEVT_DTMF_DIGIT, KERCHEVT_DTMF_END,
        KERCHEVT_QUEUE_DRAIN, KERCHEVT_QUEUE_COMPLETE, KERCHEVT_RECORDING_SAVED,
        KERCHEVT_ANNOUNCEMENT, KERCHEVT_CONFIG_RELOAD, KERCHEVT_SHUTDOWN,
    };
    for (size_t i = 0; i < sizeof(evt_types) / sizeof(evt_types[0]); i++)
        kerchevt_subscribe(evt_types[i], socket_event_handler, NULL);

    KERCHUNK_LOG_I(LOG_MOD, "listening on %s (mode 0600)", g_socket_path);
    return 0;
}

void kerchunk_socket_shutdown(void)
{
    pthread_mutex_lock(&g_client_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd >= 0) {
            close(g_clients[i].fd);
            init_slot(&g_clients[i]);
        }
    }
    pthread_mutex_unlock(&g_client_mutex);

    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    unlink(g_socket_path);
}

void kerchunk_socket_set_core_commands(const kerchunk_cli_cmd_t *cmds, int count)
{
    g_core_cmds     = cmds;
    g_num_core_cmds = count;
}

void kerchunk_socket_set_sounds_dir(const char *path)
{
    if (path)
        snprintf(g_sounds_dir, sizeof(g_sounds_dir), "%s", path);
    else
        g_sounds_dir[0] = '\0';
}

void kerchunk_socket_iter_core_commands(
    void (*cb)(const kerchunk_cli_cmd_t *cmd, void *ud),
    void *ud)
{
    if (!cb || !g_core_cmds) return;
    for (int i = 0; i < g_num_core_cmds; i++) {
        cb(&g_core_cmds[i], ud);
    }
}

void kerchunk_socket_poll(void)
{
    if (g_listen_fd < 0)
        return;

    /* Accept new connections */
    int client = accept(g_listen_fd, NULL, NULL);
    if (client >= 0) {
        if (check_peer_uid(client) < 0) {
            close(client);
        } else {
            if (fcntl(client, F_SETFL, O_NONBLOCK) < 0)
                KERCHUNK_LOG_W(LOG_MOD, "client fcntl failed");

            int slot = -1;
            pthread_mutex_lock(&g_client_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (g_clients[i].fd < 0) {
                    slot = i;
                    break;
                }
            }
            if (slot >= 0) {
                init_slot(&g_clients[slot]);
                g_clients[slot].fd = client;
            }
            pthread_mutex_unlock(&g_client_mutex);

            if (slot >= 0) {
                char welcome[64];
                snprintf(welcome, sizeof(welcome), ". kerchunkd v%s connected\n", KERCHUNK_VERSION_STRING);
                write_all(client, welcome, strlen(welcome));
            } else {
                const char *busy = "Server busy\n";
                ssize_t n = write(client, busy, strlen(busy));
                (void)n;
                close(client);
            }
        }
    }

    /* Service existing clients */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].fd < 0)
            continue;

        char buf[MAX_CMD_LEN];
        ssize_t n = read(g_clients[i].fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            /* Accumulate into cmd_buf until newline */
            client_slot_t *c = &g_clients[i];
            for (ssize_t j = 0; j < n; j++) {
                if (c->cmd_len >= MAX_CMD_LEN - 1) {
                    c->cmd_len = 0;
                    continue;
                }
                c->cmd_buf[c->cmd_len++] = buf[j];
                if (buf[j] == '\n') {
                    c->cmd_buf[c->cmd_len - 1] = '\0';
                    if (c->cmd_len > 1)
                        handle_command(c, c->cmd_buf);
                    c->cmd_len = 0;
                }
            }
        } else if (n == 0) {
            pthread_mutex_lock(&g_client_mutex);
            close(g_clients[i].fd);
            init_slot(&g_clients[i]);
            pthread_mutex_unlock(&g_client_mutex);
        }
    }
}
