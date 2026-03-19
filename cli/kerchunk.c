/*
 * kerchunk.c — Interactive CLI console for kerchunkd
 *
 * Dual-mode: one-shot command execution or interactive console
 * with tab completion, log streaming, and line editing via linenoise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

#include "linenoise.h"

#define DEFAULT_SOCKET "/tmp/kerchunk.sock"
#define MAX_RESP       8192
#define MAX_CMDS       128
#define HISTORY_FILE   ".kerchunk_history"

/* ── Command table (populated from __COMMANDS__) ────────────────────── */

typedef struct {
    char name[8192];
    char usage[128];
    char description[128];
} cmd_entry_t;

static cmd_entry_t g_cmd_table[MAX_CMDS];
static int         g_num_cmds;
static int         g_json_mode;

/* ── Reader thread shared state ─────────────────────────────────────── */

static pthread_mutex_t g_resp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_resp_cond  = PTHREAD_COND_INITIALIZER;
static int             g_resp_ready;
static char            g_resp_buf[MAX_RESP];
static int             g_resp_len;
static int             g_disconnected;
static int             g_sock_fd = -1;

/* ── Socket helpers ─────────────────────────────────────────────────── */

static int connect_to_daemon(const char *socket_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        if (errno == ENOENT || errno == ECONNREFUSED)
            fprintf(stderr, "Error: kerchunkd not running (socket %s)\n",
                    socket_path);
        else
            perror("connect");
        close(fd);
        return -1;
    }

    return fd;
}

static void send_line(int fd, const char *line)
{
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s\n", line);
    ssize_t w = write(fd, buf, (size_t)n);
    (void)w;
}

/* Read one line (up to \n) from fd, strip the \n. Returns -1 on EOF/error. */
static int read_line(int fd, char *buf, size_t max)
{
    size_t pos = 0;
    while (pos < max - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n <= 0) return -1;
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            return 0;
        }
        pos++;
    }
    buf[pos] = '\0';
    return 0;
}

/* ── One-shot mode ──────────────────────────────────────────────────── */

static int oneshot_mode(const char *socket_path, int argc, char **argv)
{
    int fd = connect_to_daemon(socket_path);
    if (fd < 0) return 1;

    /* Read and discard welcome */
    char welcome[256];
    read_line(fd, welcome, sizeof(welcome));

    /* Build and send command */
    char cmd[1024] = "";
    for (int i = 0; i < argc; i++) {
        if (i > 0) strcat(cmd, " ");
        strncat(cmd, argv[i], sizeof(cmd) - strlen(cmd) - 2);
    }
    send_line(fd, cmd);

    /* Read framed response: R lines until . */
    char line[4096];

    while (read_line(fd, line, sizeof(line)) == 0) {
        if (line[0] == '.' && (line[1] == '\0' || line[1] == ' '))
            break;
        if (line[0] == 'R' && line[1] == ' ')
            printf("%s\n", line + 2);
    }

    close(fd);
    return 0;
}

/* ── Reader thread ──────────────────────────────────────────────────── */

static void *reader_thread_fn(void *arg)
{
    (void)arg;
    char line[4096];

    while (!g_disconnected) {
        if (read_line(g_sock_fd, line, sizeof(line)) < 0) {
            pthread_mutex_lock(&g_resp_mutex);
            g_disconnected = 1;
            g_resp_ready = 1;
            pthread_cond_signal(&g_resp_cond);
            pthread_mutex_unlock(&g_resp_mutex);
            break;
        }

        if (line[0] == 'R' && line[1] == ' ') {
            /* Response line — append to buffer */
            pthread_mutex_lock(&g_resp_mutex);
            int rem = MAX_RESP - g_resp_len - 1;
            if (rem > 0) {
                int n = snprintf(g_resp_buf + g_resp_len, (size_t)rem,
                                 "%s\n", line + 2);
                if (n > 0) g_resp_len += (n < rem) ? n : rem;
            }
            pthread_mutex_unlock(&g_resp_mutex);
        } else if (line[0] == '.' && (line[1] == '\0' || line[1] == ' ')) {
            /* End of response */
            pthread_mutex_lock(&g_resp_mutex);
            g_resp_ready = 1;
            pthread_cond_signal(&g_resp_cond);
            pthread_mutex_unlock(&g_resp_mutex);
        } else if (line[0] == 'E' && line[1] == ' ') {
            /* Async event — clear line, print dim, refresh prompt */
            fprintf(stderr, "\r\033[K\033[90m%s\033[0m\n", line + 2);
            linenoiseRefreshLine();
        }
    }

    return NULL;
}

/* ── Send command, wait for framed response ─────────────────────────── */

static int send_and_wait(const char *cmd, char *resp, size_t max)
{
    if (g_disconnected || g_sock_fd < 0) {
        resp[0] = '\0';
        return -1;
    }

    pthread_mutex_lock(&g_resp_mutex);
    g_resp_ready = 0;
    g_resp_len = 0;
    g_resp_buf[0] = '\0';
    pthread_mutex_unlock(&g_resp_mutex);

    send_line(g_sock_fd, cmd);

    pthread_mutex_lock(&g_resp_mutex);
    while (!g_resp_ready && !g_disconnected)
        pthread_cond_wait(&g_resp_cond, &g_resp_mutex);

    if (g_disconnected) {
        pthread_mutex_unlock(&g_resp_mutex);
        return -1;
    }

    size_t len = (size_t)g_resp_len;
    if (len >= max) len = max - 1;
    memcpy(resp, g_resp_buf, len);
    resp[len] = '\0';
    pthread_mutex_unlock(&g_resp_mutex);
    return 0;
}

/* ── Command discovery ──────────────────────────────────────────────── */

static void load_commands(void)
{
    char resp[MAX_RESP];
    if (send_and_wait("__COMMANDS__", resp, sizeof(resp)) < 0)
        return;

    g_num_cmds = 0;
    char *p = resp;
    while (*p && g_num_cmds < MAX_CMDS) {
        char *eol = strchr(p, '\n');
        if (!eol) break;
        *eol = '\0';

        /* Parse tab-separated: name\tusage\tdescription */
        char *tab1 = strchr(p, '\t');
        if (tab1) {
            *tab1 = '\0';
            char *tab2 = strchr(tab1 + 1, '\t');
            if (tab2) {
                *tab2 = '\0';
                snprintf(g_cmd_table[g_num_cmds].description,
                         sizeof(g_cmd_table[0].description), "%s", tab2 + 1);
            } else {
                g_cmd_table[g_num_cmds].description[0] = '\0';
            }
            snprintf(g_cmd_table[g_num_cmds].usage,
                     sizeof(g_cmd_table[0].usage), "%s", tab1 + 1);
        } else {
            g_cmd_table[g_num_cmds].usage[0] = '\0';
            g_cmd_table[g_num_cmds].description[0] = '\0';
        }

        snprintf(g_cmd_table[g_num_cmds].name,
                 sizeof(g_cmd_table[0].name), "%s", p);
        g_num_cmds++;
        p = eol + 1;
    }
}

/* ── Tab completion ─────────────────────────────────────────────────── */

static const char *g_local_cmds[] = {
    "help", "show", "exit", "quit",
    "/help", "/log ", "/nolog", "/reconnect", "/exit", "/quit",
    NULL
};

static void completion_callback(const char *buf, linenoiseCompletions *lc)
{
    size_t len = strlen(buf);
    /* Daemon commands */
    for (int i = 0; i < g_num_cmds; i++) {
        if (strncmp(buf, g_cmd_table[i].name, len) == 0)
            linenoiseAddCompletion(lc, g_cmd_table[i].name);
    }
    /* Local/console commands */
    for (int i = 0; g_local_cmds[i]; i++) {
        if (strncmp(buf, g_local_cmds[i], len) == 0)
            linenoiseAddCompletion(lc, g_local_cmds[i]);
    }
}

/* ── Help display ───────────────────────────────────────────────────── */

static void show_help(const char *topic)
{
    if (topic && *topic) {
        /* Help for a specific command */
        for (int i = 0; i < g_num_cmds; i++) {
            if (strcmp(topic, g_cmd_table[i].name) == 0) {
                printf("  %-12s %s\n", "Command:", g_cmd_table[i].name);
                printf("  %-12s %s\n", "Usage:", g_cmd_table[i].usage);
                printf("  %-12s %s\n", "Description:",
                       g_cmd_table[i].description);
                return;
            }
        }
        printf("Unknown command: %s\n", topic);
        return;
    }

    printf("%-20s %-35s %s\n", "COMMAND", "USAGE", "DESCRIPTION");
    printf("%-20s %-35s %s\n", "-------", "-----", "-----------");
    for (int i = 0; i < g_num_cmds; i++)
        printf("%-20s %-35s %s\n",
               g_cmd_table[i].name,
               g_cmd_table[i].usage,
               g_cmd_table[i].description);
    printf("\nConsole commands:\n");
    printf("  help [command]     Show help (or help for a specific command)\n");
    printf("  show               Alias for 'status'\n");
    printf("  /log <level>       Start log streaming "
           "(error|warn|info|debug)\n");
    printf("  /nolog             Stop log streaming\n");
    printf("  /reconnect         Reconnect to daemon\n");
    printf("  exit, quit         Exit console\n");
}

/* ── Internal / commands ────────────────────────────────────────────── */

/* Returns: 0 = continue, -1 = exit, 1 = reconnect */
static int handle_internal_cmd(const char *line)
{
    if (strcmp(line, "/exit") == 0 || strcmp(line, "/quit") == 0)
        return -1;

    if (strcmp(line, "/help") == 0) {
        show_help(NULL);
        return 0;
    }

    if (strncmp(line, "/log ", 5) == 0) {
        const char *lvl = line + 5;
        int level = 6;
        if (strcmp(lvl, "error") == 0)      level = 3;
        else if (strcmp(lvl, "warn") == 0)  level = 4;
        else if (strcmp(lvl, "info") == 0)  level = 6;
        else if (strcmp(lvl, "debug") == 0) level = 7;
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "__SUBSCRIBE__ %d", level);
        char resp[256];
        send_and_wait(cmd, resp, sizeof(resp));
        printf("Log streaming: %s (level %d)\n", lvl, level);
        return 0;
    }

    if (strcmp(line, "/log") == 0) {
        char resp[256];
        send_and_wait("__SUBSCRIBE__ 6", resp, sizeof(resp));
        printf("Log streaming: info (level 6)\n");
        return 0;
    }

    if (strcmp(line, "/nolog") == 0) {
        char resp[256];
        send_and_wait("__UNSUBSCRIBE__", resp, sizeof(resp));
        printf("Log streaming stopped\n");
        return 0;
    }

    if (strcmp(line, "/reconnect") == 0)
        return 1;

    printf("Unknown command: %s\n", line);
    return 0;
}

/* ── Session lifecycle ──────────────────────────────────────────────── */

static int start_session(const char *socket_path, pthread_t *reader_tid)
{
    g_sock_fd = connect_to_daemon(socket_path);
    if (g_sock_fd < 0)
        return -1;

    /* Read welcome */
    char welcome[256];
    read_line(g_sock_fd, welcome, sizeof(welcome));
    if (welcome[0] == '.' && welcome[1] == ' ')
        printf("%s\n", welcome + 2);

    /* Reset reader state */
    g_disconnected = 0;
    g_resp_ready = 0;
    g_resp_len = 0;

    if (pthread_create(reader_tid, NULL, reader_thread_fn, NULL) != 0) {
        fprintf(stderr, "Error: failed to create reader thread\n");
        close(g_sock_fd);
        g_sock_fd = -1;
        return -1;
    }

    /* Load command table for tab completion */
    load_commands();

    /* Auto-subscribe to info-level logs */
    {
        char resp[256];
        send_and_wait("__SUBSCRIBE__ 6", resp, sizeof(resp));
    }

    return 0;
}

static void stop_session(pthread_t reader_tid)
{
    if (!g_disconnected && g_sock_fd >= 0) {
        send_line(g_sock_fd, "__UNSUBSCRIBE__");
        shutdown(g_sock_fd, SHUT_RDWR);
    }
    pthread_join(reader_tid, NULL);
    if (g_sock_fd >= 0) {
        close(g_sock_fd);
        g_sock_fd = -1;
    }
}

/* ── Interactive mode ───────────────────────────────────────────────── */

static int interactive_mode(const char *socket_path)
{
    pthread_t reader_tid;

    if (start_session(socket_path, &reader_tid) < 0)
        return 1;

    linenoiseSetCompletionCallback(completion_callback);

    /* History */
    char hist_path[512];
    const char *home = getenv("HOME");
    if (home)
        snprintf(hist_path, sizeof(hist_path), "%s/%s", home, HISTORY_FILE);
    else
        snprintf(hist_path, sizeof(hist_path), "%s", HISTORY_FILE);
    linenoiseHistoryLoad(hist_path);
    linenoiseHistorySetMaxLen(200);

    /* Main loop */
    int user_quit = 0;
    while (!user_quit) {
        /* Auto-reconnect loop */
        while (g_disconnected) {
            stop_session(reader_tid);
            fprintf(stderr, "\nDisconnected — reconnecting");
            int reconnected = 0;
            for (int attempt = 0; attempt < 30; attempt++) {
                fprintf(stderr, ".");
                fflush(stderr);
                struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
                nanosleep(&ts, NULL);
                if (start_session(socket_path, &reader_tid) == 0) {
                    fprintf(stderr, " connected!\n");
                    reconnected = 1;
                    break;
                }
            }
            if (!reconnected) {
                fprintf(stderr, " gave up after 60s\n");
                linenoiseHistorySave(hist_path);
                return 1;
            }
        }

        char *line = linenoise("kerchunk> ");
        if (!line) break;  /* EOF / Ctrl-D */

        if (line[0] == '\0') {
            linenoiseFree(line);
            continue;
        }

        /* Internal commands */
        if (line[0] == '/') {
            int rc = handle_internal_cmd(line);
            linenoiseHistoryAdd(line);
            linenoiseFree(line);

            if (rc < 0) { user_quit = 1; break; }

            if (rc == 1) {
                /* /reconnect */
                stop_session(reader_tid);
                if (start_session(socket_path, &reader_tid) < 0) {
                    fprintf(stderr, "Reconnect failed\n");
                    linenoiseHistorySave(hist_path);
                    return 1;
                }
                printf("Reconnected\n");
            }
            continue;
        }

        /* Client-side commands (no slash prefix) */
        if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
            show_help(NULL);
            linenoiseHistoryAdd(line);
            linenoiseFree(line);
            continue;
        }
        if (strncmp(line, "help ", 5) == 0) {
            show_help(line + 5);
            linenoiseHistoryAdd(line);
            linenoiseFree(line);
            continue;
        }
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
            linenoiseFree(line);
            user_quit = 1;
            break;
        }
        if (strcmp(line, "show") == 0) {
            linenoiseFree(line);
            line = NULL;
            char resp[MAX_RESP];
            if (send_and_wait("status", resp, sizeof(resp)) < 0)
                continue;  /* disconnected — loop will reconnect */
            if (resp[0])
                fputs(resp, stdout);
            linenoiseHistoryAdd("show");
            continue;
        }

        /* Send command to daemon */
        char resp[MAX_RESP];
        if (send_and_wait(line, resp, sizeof(resp)) < 0) {
            linenoiseFree(line);
            continue;  /* disconnected — loop will reconnect */
        }

        if (resp[0])
            fputs(resp, stdout);

        linenoiseHistoryAdd(line);
        linenoiseFree(line);
    }

    linenoiseHistorySave(hist_path);
    stop_session(reader_tid);
    return 0;
}

/* ── Entry point ────────────────────────────────────────────────────── */

/* ── Event stream mode ──────────────────────────────────────────────── */

static int event_stream_mode(const char *socket_path, int json)
{
    int fd = connect_to_daemon(socket_path);
    if (fd < 0) return 1;

    char welcome[256];
    read_line(fd, welcome, sizeof(welcome));

    /* Subscribe: structured JSON events or text log */
    send_line(fd, json ? "__JSON_EVENTS__" : "__SUBSCRIBE__ 7");
    char ack[256];
    read_line(fd, ack, sizeof(ack));

    /* Stream until EOF or SIGINT */
    char line[4096];
    while (read_line(fd, line, sizeof(line)) == 0) {
        if (line[0] == 'E' && line[1] == ' ') {
            if (json)
                printf("%s\n", line + 2);  /* Already JSON from server */
            else
                printf("%s\n", line + 2);
            fflush(stdout);
        }
    }

    close(fd);
    return 0;
}

/* ── Entry point ────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr, "Usage: kerchunk [-s socket] [-j] [-e] [-x command] "
                    "[command args...]\n");
    fprintf(stderr, "\nWith no command, starts interactive console.\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -s socket   Socket path (default: %s)\n",
            DEFAULT_SOCKET);
    fprintf(stderr, "  -x command  Execute command and exit (scriptable)\n");
    fprintf(stderr, "  -j          JSON output (for -x or positional commands)\n");
    fprintf(stderr, "  -e          Stream events (combine with -j for NDJSON)\n");
    fprintf(stderr, "  -h          Show this help\n");
    fprintf(stderr, "\nExamples:\n");
    fprintf(stderr, "  kerchunk                           Interactive console\n");
    fprintf(stderr, "  kerchunk status                    One-shot command\n");
    fprintf(stderr, "  kerchunk -j status                 JSON output\n");
    fprintf(stderr, "  kerchunk -j -x 'stats'             JSON from script\n");
    fprintf(stderr, "  kerchunk -e                        Stream events (text)\n");
    fprintf(stderr, "  kerchunk -e -j                     Stream events (NDJSON)\n");
    fprintf(stderr, "  kerchunk -e -j | jq .              Pretty-print events\n");
}

int main(int argc, char **argv)
{
    const char *socket_path = DEFAULT_SOCKET;
    const char *exec_cmd = NULL;
    int event_mode = 0;
    g_json_mode = 0;

    signal(SIGPIPE, SIG_IGN);

    /* Parse options */
    int opt_end = 1;
    while (opt_end < argc && argv[opt_end][0] == '-') {
        if (strcmp(argv[opt_end], "-s") == 0 && opt_end + 1 < argc) {
            socket_path = argv[opt_end + 1];
            opt_end += 2;
        } else if (strcmp(argv[opt_end], "-x") == 0 && opt_end + 1 < argc) {
            exec_cmd = argv[opt_end + 1];
            opt_end += 2;
        } else if (strcmp(argv[opt_end], "-j") == 0) {
            g_json_mode = 1;
            opt_end++;
        } else if (strcmp(argv[opt_end], "-e") == 0) {
            event_mode = 1;
            opt_end++;
        } else if (strcmp(argv[opt_end], "-h") == 0 ||
                   strcmp(argv[opt_end], "--help") == 0) {
            usage();
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[opt_end]);
            usage();
            return 1;
        }
    }

    if (event_mode)
        return event_stream_mode(socket_path, g_json_mode);

    if (exec_cmd) {
        if (g_json_mode) {
            char jcmd[1100];
            snprintf(jcmd, sizeof(jcmd), "__JSON__ %s", exec_cmd);
            return oneshot_mode(socket_path, 1, (char *[]){ jcmd });
        }
        return oneshot_mode(socket_path, 1, (char *[]){ (char *)exec_cmd });
    }

    if (opt_end < argc) {
        if (g_json_mode) {
            char jcmd[1100] = "__JSON__ ";
            for (int i = opt_end; i < argc; i++) {
                if (i > opt_end) strcat(jcmd, " ");
                strncat(jcmd, argv[i], sizeof(jcmd) - strlen(jcmd) - 2);
            }
            return oneshot_mode(socket_path, 1, (char *[]){ jcmd });
        }
        return oneshot_mode(socket_path, argc - opt_end, argv + opt_end);
    }

    return interactive_mode(socket_path);
}
