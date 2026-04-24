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
#include <sys/select.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

#include "linenoise.h"

#define DEFAULT_SOCKET "/run/kerchunk/kerchunk.sock"
#define MAX_RESP       16384
#define MAX_CMDS       128
#define HISTORY_FILE   ".kerchunk_history"

/* ── Command table (populated from __COMMANDS__) ────────────────────── */

typedef struct {
    char name[64];
    char usage[128];
    char description[128];
} cmd_entry_t;

static cmd_entry_t g_cmd_table[MAX_CMDS];
static int         g_num_cmds;
static int         g_json_mode;

/* ── Completion table (populated from __COMPLETIONS__) ─────────────── */

#define MAX_COMPLETIONS    256
#define MAX_COMPL_FIELDS   8

typedef struct {
    char name[32];
    char type[16];        /* "text", "number", "select", "file", "path", "dynamic:..." */
    char options[256];    /* comma-separated for select; lookup key for dynamic */
} completion_field_t;

typedef struct {
    char cmd[32];                                /* first word */
    char sub[32];                                /* subcommand or "" */
    completion_field_t fields[MAX_COMPL_FIELDS];
    int  num_fields;
} completion_entry_t;

static completion_entry_t g_completions[MAX_COMPLETIONS];
static int                g_num_completions;

/* Dynamic lists fetched at session start (refreshed on /reconnect) */
#define MAX_DYNAMIC_ITEMS 256
typedef struct {
    char items[MAX_DYNAMIC_ITEMS][64];
    int  count;
} dynamic_list_t;

static dynamic_list_t g_users;
static dynamic_list_t g_modules;
static dynamic_list_t g_sounds;

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
                 sizeof(g_cmd_table[0].name), "%.*s",
                 (int)(sizeof(g_cmd_table[0].name) - 1), p);
        g_num_cmds++;
        p = eol + 1;
    }
}

/* ── Completion discovery ───────────────────────────────────────────── */

/* Parse one tab-separated row from __COMPLETIONS__:
 *   <cmd>\t<sub>\t<field1_name>:<type>:<options>\t<field2>... */
static void parse_completion_row(char *line)
{
    if (g_num_completions >= MAX_COMPLETIONS) return;

    completion_entry_t *e = &g_completions[g_num_completions];
    memset(e, 0, sizeof(*e));

    char *tab = strchr(line, '\t');
    if (!tab) {
        /* Bare command name with no subcommand */
        snprintf(e->cmd, sizeof(e->cmd), "%.*s",
                 (int)(sizeof(e->cmd) - 1), line);
        g_num_completions++;
        return;
    }
    *tab = '\0';
    snprintf(e->cmd, sizeof(e->cmd), "%.*s",
             (int)(sizeof(e->cmd) - 1), line);

    char *sub_start = tab + 1;
    char *next = strchr(sub_start, '\t');
    if (next) {
        *next = '\0';
        snprintf(e->sub, sizeof(e->sub), "%.*s",
                 (int)(sizeof(e->sub) - 1), sub_start);
        sub_start = next + 1;
    } else {
        snprintf(e->sub, sizeof(e->sub), "%.*s",
                 (int)(sizeof(e->sub) - 1), sub_start);
        g_num_completions++;
        return;
    }

    /* Remaining tabs are field specs */
    while (sub_start && *sub_start && e->num_fields < MAX_COMPL_FIELDS) {
        next = strchr(sub_start, '\t');
        if (next) *next = '\0';

        completion_field_t *f = &e->fields[e->num_fields];
        char *colon1 = strchr(sub_start, ':');
        if (colon1) {
            *colon1 = '\0';
            snprintf(f->name, sizeof(f->name), "%s", sub_start);
            char *colon2 = strchr(colon1 + 1, ':');
            if (colon2) {
                *colon2 = '\0';
                snprintf(f->type, sizeof(f->type), "%s", colon1 + 1);
                snprintf(f->options, sizeof(f->options), "%s", colon2 + 1);
            } else {
                snprintf(f->type, sizeof(f->type), "%s", colon1 + 1);
            }
        } else {
            snprintf(f->name, sizeof(f->name), "%s", sub_start);
            snprintf(f->type, sizeof(f->type), "text");
        }
        e->num_fields++;

        sub_start = next ? next + 1 : NULL;
    }

    g_num_completions++;
}

static void load_completions(void)
{
    char resp[MAX_RESP];
    if (send_and_wait("__COMPLETIONS__", resp, sizeof(resp)) < 0)
        return;

    g_num_completions = 0;
    char *p = resp;
    while (*p && g_num_completions < MAX_COMPLETIONS) {
        char *eol = strchr(p, '\n');
        if (!eol) break;
        *eol = '\0';
        if (*p) parse_completion_row(p);
        p = eol + 1;
    }
}

/* Fetch a dynamic list (newline-separated) into the given dynamic_list_t */
static void load_dynamic_list(const char *meta_cmd, dynamic_list_t *out)
{
    out->count = 0;
    char resp[MAX_RESP];
    if (send_and_wait(meta_cmd, resp, sizeof(resp)) < 0) return;

    char *p = resp;
    while (*p && out->count < MAX_DYNAMIC_ITEMS) {
        char *eol = strchr(p, '\n');
        if (!eol) break;
        *eol = '\0';
        if (*p)
            snprintf(out->items[out->count++], sizeof(out->items[0]), "%.63s", p);
        p = eol + 1;
    }
}

static void load_dynamic_lists(void)
{
    load_dynamic_list("__USERS__", &g_users);
    load_dynamic_list("__MODULES__", &g_modules);
    load_dynamic_list("__SOUNDS__", &g_sounds);
}

/* ── Tab completion ─────────────────────────────────────────────────── */

static const char *g_local_cmds[] = {
    "help", "show", "exit", "quit",
    "/help", "/log ", "/nolog", "/reconnect", "/exit", "/quit",
    NULL
};

#define MAX_TOKENS 16
#define TOKEN_LEN  64

/* Tokenize buf into words. Returns number of complete words.
 * Sets *trailing_space to 1 if buf ends with whitespace. */
static int tokenize_line(const char *buf,
                         char tokens[MAX_TOKENS][TOKEN_LEN],
                         int *trailing_space)
{
    int n = 0;
    const char *p = buf;
    while (*p && n < MAX_TOKENS) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        if (len >= TOKEN_LEN) len = TOKEN_LEN - 1;
        memcpy(tokens[n], start, len);
        tokens[n][len] = '\0';
        n++;
    }
    size_t blen = strlen(buf);
    *trailing_space = (blen > 0 && (buf[blen - 1] == ' ' || buf[blen - 1] == '\t'));
    return n;
}

/* Build prefix string: tokens 0..(end-1) joined by spaces, with trailing space. */
static void build_prefix(char tokens[MAX_TOKENS][TOKEN_LEN], int end,
                         char *out, size_t out_size)
{
    size_t off = 0;
    out[0] = '\0';
    for (int i = 0; i < end && off + 1 < out_size; i++) {
        int n = snprintf(out + off, out_size - off, "%s ", tokens[i]);
        if (n < 0) break;
        off += (size_t)n;
        if (off >= out_size) { off = out_size - 1; break; }
    }
}

static void add_full_completion(linenoiseCompletions *lc,
                                const char *prefix, const char *suffix)
{
    char full[1024];
    snprintf(full, sizeof(full), "%s%s", prefix, suffix);
    linenoiseAddCompletion(lc, full);
}

/* Complete the current word against a cached dynamic list */
static void complete_from_dynamic_list(linenoiseCompletions *lc,
                                       const char *prefix, const char *current,
                                       const dynamic_list_t *list)
{
    size_t curlen = strlen(current);
    for (int i = 0; i < list->count; i++) {
        if (strncmp(current, list->items[i], curlen) == 0)
            add_full_completion(lc, prefix, list->items[i]);
    }
}

/* Filesystem path completion: split current into dirname/basename, list dirname,
 * and emit matches whose names start with basename. */
static void complete_filesystem_path(linenoiseCompletions *lc,
                                     const char *prefix, const char *current)
{
    char dir[512];
    const char *base;
    const char *slash = strrchr(current, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - current) + 1;  /* include trailing / */
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, current, dlen);
        dir[dlen] = '\0';
        base = slash + 1;
    } else {
        snprintf(dir, sizeof(dir), "./");
        base = current;
    }

    DIR *d = opendir(dir[0] == '/' ? dir : dir);
    if (!d) return;

    size_t blen = strlen(base);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' && base[0] != '.') continue;
        if (strncmp(de->d_name, base, blen) != 0) continue;

        /* Build completion: prefix + dir-portion + entry name */
        char suffix[512];
        if (slash)
            snprintf(suffix, sizeof(suffix), "%.*s%s",
                     (int)(slash - current + 1), current, de->d_name);
        else
            snprintf(suffix, sizeof(suffix), "%s", de->d_name);

        /* Append / for directories so user can keep tabbing */
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s",
                 dir[0] == '/' ? dir : ".", de->d_name);
        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode))
            strncat(suffix, "/", sizeof(suffix) - strlen(suffix) - 1);

        add_full_completion(lc, prefix, suffix);
    }
    closedir(d);
}

static void completion_callback(const char *buf, linenoiseCompletions *lc)
{
    /* Slash commands stay on local list, first-word only */
    if (buf[0] == '/') {
        size_t len = strlen(buf);
        for (int i = 0; g_local_cmds[i]; i++) {
            if (g_local_cmds[i][0] == '/' &&
                strncmp(buf, g_local_cmds[i], len) == 0)
                linenoiseAddCompletion(lc, g_local_cmds[i]);
        }
        return;
    }

    char tokens[MAX_TOKENS][TOKEN_LEN];
    int trailing_space;
    int num_tokens = tokenize_line(buf, tokens, &trailing_space);

    int word_idx;
    const char *current;
    if (trailing_space || num_tokens == 0) {
        word_idx = num_tokens;
        current = "";
    } else {
        word_idx = num_tokens - 1;
        current = tokens[word_idx];
    }
    size_t curlen = strlen(current);

    char prefix[512];
    build_prefix(tokens, word_idx, prefix, sizeof(prefix));

    /* ── Word 0: command names ─────────────────────────────────── */
    if (word_idx == 0) {
        for (int i = 0; i < g_num_cmds; i++) {
            if (strncmp(current, g_cmd_table[i].name, curlen) == 0)
                linenoiseAddCompletion(lc, g_cmd_table[i].name);
        }
        for (int i = 0; g_local_cmds[i]; i++) {
            if (g_local_cmds[i][0] == '/') continue;
            if (strncmp(current, g_local_cmds[i], curlen) == 0)
                linenoiseAddCompletion(lc, g_local_cmds[i]);
        }
        return;
    }

    /* ── Word 1: subcommand of tokens[0] ───────────────────────── */
    if (word_idx == 1) {
        const char *cmd = tokens[0];
        char seen[32][32];
        int seen_count = 0;

        for (int i = 0; i < g_num_completions; i++) {
            if (strcmp(g_completions[i].cmd, cmd) != 0) continue;
            if (g_completions[i].sub[0] == '\0') continue;
            if (strncmp(current, g_completions[i].sub, curlen) != 0) continue;

            int dup = 0;
            for (int j = 0; j < seen_count; j++) {
                if (strcmp(seen[j], g_completions[i].sub) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
            if (seen_count < 32)
                snprintf(seen[seen_count++], 32, "%s", g_completions[i].sub);

            add_full_completion(lc, prefix, g_completions[i].sub);
        }
        return;
    }

    /* ── Word ≥2: argument completion via field type ──────────── */
    const char *cmd = tokens[0];
    const char *sub = tokens[1];
    int field_idx = word_idx - 2;

    for (int i = 0; i < g_num_completions; i++) {
        if (strcmp(g_completions[i].cmd, cmd) != 0) continue;
        if (strcmp(g_completions[i].sub, sub) != 0) continue;
        if (field_idx >= g_completions[i].num_fields) continue;

        completion_field_t *f = &g_completions[i].fields[field_idx];

        if (strcmp(f->type, "select") == 0 && f->options[0]) {
            const char *o = f->options;
            while (*o) {
                const char *e = strchr(o, ',');
                size_t olen = e ? (size_t)(e - o) : strlen(o);
                char opt[64];
                if (olen >= sizeof(opt)) olen = sizeof(opt) - 1;
                memcpy(opt, o, olen);
                opt[olen] = '\0';
                if (strncmp(current, opt, curlen) == 0)
                    add_full_completion(lc, prefix, opt);
                o = e ? e + 1 : o + olen;
            }
        } else if (strcmp(f->type, "user") == 0) {
            complete_from_dynamic_list(lc, prefix, current, &g_users);
        } else if (strcmp(f->type, "module") == 0) {
            complete_from_dynamic_list(lc, prefix, current, &g_modules);
        } else if (strcmp(f->type, "sound") == 0) {
            complete_from_dynamic_list(lc, prefix, current, &g_sounds);
        } else if (strcmp(f->type, "file") == 0 ||
                   strcmp(f->type, "path") == 0) {
            complete_filesystem_path(lc, prefix, current);
        }
        break;
    }
}

/* ── Hints (dim text after cursor) ──────────────────────────────────── */

static char g_hint_buf[256];

static char *hints_callback(const char *buf, int *color, int *bold)
{
    *color = 90;  /* dim grey */
    *bold = 0;

    size_t blen = strlen(buf);
    if (blen == 0 || buf[0] == '/') return NULL;
    /* Only show hints when the cursor sits after a complete word */
    if (buf[blen - 1] != ' ') return NULL;

    char tokens[MAX_TOKENS][TOKEN_LEN];
    int trailing_space;
    int num_tokens = tokenize_line(buf, tokens, &trailing_space);
    if (num_tokens == 0) return NULL;

    /* Hint after first word: list available subcommands */
    if (num_tokens == 1) {
        const char *cmd = tokens[0];
        char list[200] = "";
        int count = 0;
        for (int i = 0; i < g_num_completions && count < 6; i++) {
            if (strcmp(g_completions[i].cmd, cmd) != 0) continue;
            if (g_completions[i].sub[0] == '\0') continue;
            int dup = 0;
            for (int j = 0; j < i; j++) {
                if (strcmp(g_completions[j].cmd, cmd) == 0 &&
                    strcmp(g_completions[j].sub, g_completions[i].sub) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;
            strncat(list, count == 0 ? "" : "|", sizeof(list) - strlen(list) - 1);
            strncat(list, g_completions[i].sub, sizeof(list) - strlen(list) - 1);
            count++;
        }
        if (count == 0) return NULL;
        snprintf(g_hint_buf, sizeof(g_hint_buf), "<%s>", list);
        return g_hint_buf;
    }

    /* Hint after subcommand and beyond: show next field */
    const char *cmd = tokens[0];
    const char *sub = tokens[1];
    int field_idx = num_tokens - 2;

    for (int i = 0; i < g_num_completions; i++) {
        if (strcmp(g_completions[i].cmd, cmd) != 0) continue;
        if (strcmp(g_completions[i].sub, sub) != 0) continue;
        if (field_idx >= g_completions[i].num_fields) continue;

        completion_field_t *f = &g_completions[i].fields[field_idx];
        if (strcmp(f->type, "select") == 0 && f->options[0])
            snprintf(g_hint_buf, sizeof(g_hint_buf), "<%s: %s>",
                     f->name, f->options);
        else
            snprintf(g_hint_buf, sizeof(g_hint_buf), "<%s>", f->name);
        return g_hint_buf;
    }
    return NULL;
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

    /* Load command table and structured completion table for tab completion */
    load_commands();
    load_completions();
    load_dynamic_lists();

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
    linenoiseSetHintsCallback(hints_callback);

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

        /* Async edit mode: poll stdin AND the g_disconnected flag
         * so a daemon shutdown drops us into the reconnect loop
         * within ~200 ms instead of waiting for the user to press
         * Enter. linenoise's blocking linenoise() call would sit
         * on read(stdin) forever otherwise. */
        struct linenoiseState ls;
        char buf[4096];
        if (linenoiseEditStart(&ls, -1, -1, buf, sizeof(buf), "kerchunk> ") < 0)
            break;

        char *line = NULL;
        while (1) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(STDIN_FILENO, &rfds);
            struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
            int n = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);

            if (g_disconnected) {
                /* Wipe the prompt before the reconnect-loop banner
                 * lands so we don't leave half-typed bytes behind. */
                linenoiseHide(&ls);
                linenoiseEditStop(&ls);
                line = linenoiseEditMore;  /* signal: jump to outer reconnect */
                break;
            }

            if (n > 0 && FD_ISSET(STDIN_FILENO, &rfds)) {
                line = linenoiseEditFeed(&ls);
                if (line == linenoiseEditMore) continue;
                linenoiseEditStop(&ls);
                break;
            }
        }

        if (line == linenoiseEditMore) continue;  /* disconnected */
        if (!line) break;                          /* EOF / Ctrl-D */

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
        /* `cmd ?` is shorthand for `help cmd` */
        {
            size_t llen = strlen(line);
            if (llen >= 2 && line[llen - 1] == '?' && line[llen - 2] == ' ') {
                char topic[64];
                snprintf(topic, sizeof(topic), "%.*s", (int)(llen - 2), line);
                show_help(topic);
                linenoiseHistoryAdd(line);
                linenoiseFree(line);
                continue;
            }
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

        /* Refresh completion + dynamic tables after commands that change
         * the loaded module set or reload config — keeps tab completion
         * in sync with the live daemon state. */
        if (strncmp(line, "module load", 11) == 0   ||
            strncmp(line, "module unload", 13) == 0 ||
            strncmp(line, "module reload", 13) == 0 ||
            strncmp(line, "config reload", 13) == 0) {
            load_commands();
            load_completions();
            load_dynamic_lists();
        }

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
