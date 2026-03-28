/*
 * kerchunk_console.c — Embedded interactive CLI console for foreground mode
 *
 * Provides an fs_cli-like experience directly in the daemon process when
 * running with -f and stdin is a terminal. Uses linenoise for line editing,
 * tab completion, and history. Commands dispatch directly through
 * kerchunk_dispatch_command() with no socket round-trip.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "kerchunk_console.h"
#include "linenoise.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>

#define LOG_MOD      "console"
#define MAX_ARGV     32
#define HISTORY_FILE ".kerchunkd_history"
#define MAX_CMDS     256

/* Daemon's running flag (declared in main.c) */
extern volatile sig_atomic_t g_running;

/* Iteration functions for tab completion */
extern void kerchunk_socket_iter_core_commands(
    void (*cb)(const char *name, const char *usage, const char *desc, void *ud),
    void *ud);

/* ── Static state ──────────────────────────────────────────────────── */

static pthread_t       g_console_tid;
static pthread_mutex_t g_console_mutex = PTHREAD_MUTEX_INITIALIZER;
static int             g_console_running;
static char            g_hist_path[512];

/* Command name table for tab completion */
static char g_cmd_names[MAX_CMDS][64];
static int  g_num_cmds;

/* ── Command tokenizer ─────────────────────────────────────────────── */

static int console_parse_cmd(char *line, const char **argv, int max_argv)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max_argv) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0')
            break;
        if (*p == '"') {
            p++;  /* skip opening quote */
            argv[argc++] = p;
            while (*p && *p != '"')
                p++;
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
        if (*p)
            *p++ = '\0';
    }
    return argc;
}

/* ── Tab completion ────────────────────────────────────────────────── */

static void collect_cmd_cb(const char *name, const char *usage,
                           const char *desc, void *ud)
{
    (void)usage; (void)desc; (void)ud;
    if (g_num_cmds < MAX_CMDS) {
        snprintf(g_cmd_names[g_num_cmds], sizeof(g_cmd_names[0]), "%s", name);
        g_num_cmds++;
    }
}

static void load_command_names(void)
{
    g_num_cmds = 0;
    kerchunk_socket_iter_core_commands(collect_cmd_cb, NULL);
    kerchunk_module_iter_cli_commands(collect_cmd_cb, NULL);
}

static const char *g_local_cmds[] = {
    "help", "show", "exit", "quit", "/quit", NULL
};

static void completion_callback(const char *buf, linenoiseCompletions *lc)
{
    size_t len = strlen(buf);
    /* Daemon commands */
    for (int i = 0; i < g_num_cmds; i++) {
        if (strncmp(buf, g_cmd_names[i], len) == 0)
            linenoiseAddCompletion(lc, g_cmd_names[i]);
    }
    /* Local console commands */
    for (int i = 0; g_local_cmds[i]; i++) {
        if (strncmp(buf, g_local_cmds[i], len) == 0)
            linenoiseAddCompletion(lc, g_local_cmds[i]);
    }
}

/* ── Help display ──────────────────────────────────────────────────── */

typedef struct {
    const char *topic;   /* NULL = show all */
    int         found;
} help_ctx_t;

static void help_show_cb(const char *name, const char *usage,
                         const char *desc, void *ud)
{
    help_ctx_t *ctx = (help_ctx_t *)ud;
    if (ctx->topic) {
        if (strcmp(ctx->topic, name) == 0) {
            printf("  %-12s %s\n", "Command:", name);
            printf("  %-12s %s\n", "Usage:", usage ? usage : name);
            printf("  %-12s %s\n", "Description:", desc ? desc : "");
            ctx->found = 1;
        }
    } else {
        printf("%-20s %-35s %s\n", name, usage ? usage : name, desc ? desc : "");
    }
}

static void show_help(const char *topic)
{
    help_ctx_t ctx = { .topic = topic, .found = 0 };

    if (!topic) {
        printf("%-20s %-35s %s\n", "COMMAND", "USAGE", "DESCRIPTION");
        printf("%-20s %-35s %s\n", "-------", "-----", "-----------");
    }

    kerchunk_socket_iter_core_commands(help_show_cb, &ctx);
    kerchunk_module_iter_cli_commands(help_show_cb, &ctx);

    if (topic && !ctx.found) {
        printf("Unknown command: %s\n", topic);
        return;
    }

    if (!topic) {
        printf("\nConsole commands:\n");
        printf("  help [command]     Show help (or help for a specific command)\n");
        printf("  show               Alias for 'status'\n");
        printf("  exit, quit         Shutdown daemon and exit console\n");
    }
}

/* ── Console thread ────────────────────────────────────────────────── */

static void *console_thread_fn(void *arg)
{
    (void)arg;

    KERCHUNK_LOG_I(LOG_MOD, "interactive console started (Ctrl-D to shutdown)");

    /* Load command names for tab completion */
    load_command_names();

    linenoiseSetCompletionCallback(completion_callback);
    linenoiseHistoryLoad(g_hist_path);
    linenoiseHistorySetMaxLen(200);

    while (g_running && g_console_running) {
        char *line = linenoise("kerchunkd> ");
        if (!line) {
            /* Ctrl-D (EOF) or Ctrl-C with errno=EAGAIN */
            if (errno == EAGAIN) {
                /* Ctrl-C — just continue */
                continue;
            }
            /* Ctrl-D — shutdown */
            printf("\n");
            g_running = 0;
            break;
        }

        /* Empty line */
        if (line[0] == '\0') {
            linenoiseFree(line);
            continue;
        }

        /* Exit/quit commands */
        if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0 ||
            strcmp(line, "/quit") == 0) {
            linenoiseHistoryAdd(line);
            linenoiseFree(line);
            g_running = 0;
            break;
        }

        /* Help */
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

        /* "show" alias for "status" */
        if (strcmp(line, "show") == 0) {
            linenoiseHistoryAdd(line);
            linenoiseFree(line);

            kerchunk_resp_t resp;
            resp_init(&resp);
            const char *argv[] = { "status" };
            kerchunk_dispatch_command(1, argv, &resp);
            resp_finish(&resp);
            if (resp.text[0])
                fputs(resp.text, stdout);
            continue;
        }

        /* Dispatch command */
        {
            /* Make a mutable copy for tokenizer */
            char cmd_copy[1024];
            snprintf(cmd_copy, sizeof(cmd_copy), "%s", line);

            const char *argv[MAX_ARGV];
            int argc = console_parse_cmd(cmd_copy, argv, MAX_ARGV);

            if (argc > 0) {
                kerchunk_resp_t resp;
                resp_init(&resp);
                kerchunk_dispatch_command(argc, argv, &resp);
                resp_finish(&resp);
                if (resp.text[0])
                    fputs(resp.text, stdout);
            }
        }

        linenoiseHistoryAdd(line);
        linenoiseFree(line);

        /* Refresh command list (modules may have been loaded/unloaded) */
        load_command_names();
    }

    linenoiseHistorySave(g_hist_path);
    KERCHUNK_LOG_I(LOG_MOD, "interactive console stopped");
    return NULL;
}

/* ── Log interleave ────────────────────────────────────────────────── */

void kerchunk_console_log_line(const char *line)
{
    pthread_mutex_lock(&g_console_mutex);
    /* Erase current prompt line, print log, let linenoise redraw on next input */
    fprintf(stderr, "\r\033[K%s\n", line);
    pthread_mutex_unlock(&g_console_mutex);
}

/* ── Public API ────────────────────────────────────────────────────── */

int kerchunk_console_init(void)
{
    /* Build history path */
    const char *home = getenv("HOME");
    if (home)
        snprintf(g_hist_path, sizeof(g_hist_path), "%s/%s", home, HISTORY_FILE);
    else
        snprintf(g_hist_path, sizeof(g_hist_path), "%s", HISTORY_FILE);

    g_console_running = 1;
    kerchunk_log_set_console_active(1);

    if (pthread_create(&g_console_tid, NULL, console_thread_fn, NULL) != 0) {
        KERCHUNK_LOG_E(LOG_MOD, "failed to create console thread");
        g_console_running = 0;
        kerchunk_log_set_console_active(0);
        return -1;
    }

    return 0;
}

void kerchunk_console_shutdown(void)
{
    if (!g_console_running)
        return;

    g_console_running = 0;
    kerchunk_log_set_console_active(0);

    /* The console thread blocks in linenoise(). It will exit when:
     * 1. g_running becomes 0 and the user presses any key (linenoise returns), or
     * 2. The thread was the one that set g_running = 0 and already exited.
     *
     * Use a timed join to avoid blocking shutdown indefinitely. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;

#ifdef __linux__
    int rc = pthread_timedjoin_np(g_console_tid, NULL, &ts);
    if (rc != 0) {
        /* Thread still blocked in linenoise — cancel it */
        pthread_cancel(g_console_tid);
        pthread_join(g_console_tid, NULL);
    }
#else
    /* Non-Linux: just cancel and join */
    pthread_cancel(g_console_tid);
    pthread_join(g_console_tid, NULL);
#endif
}
