/*
 * kerchunk_log.c — Logging implementation
 */

#include "kerchunk_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

#ifdef __linux__
#include <syslog.h>
#endif

static int  g_dest  = KERCHUNK_LOG_DEST_STDERR;
static int  g_level = KERCHUNK_LOG_INFO;
static FILE *g_fp   = NULL;
static FILE *g_tee  = NULL;   /* secondary log file (mod_logger) */
static int  g_console_active;

void kerchunk_log_tee_file(FILE *fp)  { g_tee = fp; }
void kerchunk_log_tee_remove(void)    { g_tee = NULL; }
void kerchunk_log_set_console_active(int active) { g_console_active = active; }
int  kerchunk_log_get_level(void)     { return g_level; }

static const char *level_str(int level)
{
    switch (level) {
    case KERCHUNK_LOG_ERROR: return "ERROR";
    case KERCHUNK_LOG_WARN:  return "WARN";
    case KERCHUNK_LOG_INFO:  return "INFO";
    case KERCHUNK_LOG_DEBUG: return "DEBUG";
    default:               return "???";
    }
}

int kerchunk_log_init(int dest, int level, const char *file_path)
{
    g_dest  = dest;
    g_level = level;

#ifdef __linux__
    if (dest == KERCHUNK_LOG_DEST_SYSLOG)
        openlog("kerchunkd", LOG_PID | LOG_NDELAY, LOG_DAEMON);
#endif

    if (dest == KERCHUNK_LOG_DEST_FILE && file_path) {
        g_fp = fopen(file_path, "a");
        if (!g_fp) {
            g_dest = KERCHUNK_LOG_DEST_STDERR;
            return -1;
        }
    }

    return 0;
}

void kerchunk_log_shutdown(void)
{
#ifdef __linux__
    if (g_dest == KERCHUNK_LOG_DEST_SYSLOG)
        closelog();
#endif

    if (g_fp) {
        fclose(g_fp);
        g_fp = NULL;
    }
}

void kerchunk_log_set_level(int level)
{
    g_level = level;
}

void kerchunk_log_msg(int level, const char *module, const char *fmt, ...)
{
    if (level > g_level)
        return;

    va_list ap;
    va_start(ap, fmt);

    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

#ifdef __linux__
    if (g_dest == KERCHUNK_LOG_DEST_SYSLOG) {
        syslog(level, "[%s] %s", module ? module : "-", msg);
        return;
    }
#endif

    /* Format timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    char timebuf[32];
    snprintf(timebuf, sizeof(timebuf), "%02d:%02d:%02d.%03ld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);

    /* When the embedded console is active, route stderr output through it
     * so log lines don't corrupt the linenoise prompt. */
    if (g_console_active && !g_fp) {
        extern void kerchunk_console_log_line(const char *line);
        char full[1100];
        snprintf(full, sizeof(full), "%s %-5s [%-10s] %s",
                 timebuf, level_str(level), module ? module : "-", msg);
        kerchunk_console_log_line(full);
    } else {
        FILE *out = g_fp ? g_fp : stderr;
        fprintf(out, "%s %-5s [%-10s] %s\n", timebuf, level_str(level),
                module ? module : "-", msg);
        fflush(out);
    }

    /* Tee to secondary log file if registered */
    {
        FILE *out = g_fp ? g_fp : stderr;
        if (g_tee && g_tee != out) {
            fprintf(g_tee, "%s %-5s [%-10s] %s\n", timebuf, level_str(level),
                    module ? module : "-", msg);
            fflush(g_tee);
        }
    }

    /* Broadcast to subscribed CLI clients */
    {
        extern void kerchunk_socket_broadcast_log(int level, const char *formatted_line);
        char full_line[1100];
        snprintf(full_line, sizeof(full_line), "%s %-5s [%-10s] %s",
                 timebuf, level_str(level), module ? module : "-", msg);
        kerchunk_socket_broadcast_log(level, full_line);
    }
}
