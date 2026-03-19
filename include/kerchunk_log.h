/*
 * kerchunk_log.h — Logging
 */

#ifndef KERCHUNK_LOG_H
#define KERCHUNK_LOG_H

#include <stdio.h>

/* Log levels (match syslog) */
#define KERCHUNK_LOG_ERROR   3
#define KERCHUNK_LOG_WARN    4
#define KERCHUNK_LOG_INFO    6
#define KERCHUNK_LOG_DEBUG   7

/* Log destinations */
#define KERCHUNK_LOG_DEST_STDERR  0
#define KERCHUNK_LOG_DEST_SYSLOG  1
#define KERCHUNK_LOG_DEST_FILE    2

/* Initialize logging. Returns 0 on success. */
int  kerchunk_log_init(int dest, int level, const char *file_path);

/* Shutdown logging. */
void kerchunk_log_shutdown(void);

/* Set log level at runtime. */
void kerchunk_log_set_level(int level);

/* Log a message. */
void kerchunk_log_msg(int level, const char *module, const char *fmt, ...);

/* Register/remove a secondary log file (tee output to both stderr and file) */
void kerchunk_log_tee_file(FILE *fp);
void kerchunk_log_tee_remove(void);

/* Convenience macros */
#define KERCHUNK_LOG_E(mod, ...) kerchunk_log_msg(KERCHUNK_LOG_ERROR, mod, __VA_ARGS__)
#define KERCHUNK_LOG_W(mod, ...) kerchunk_log_msg(KERCHUNK_LOG_WARN, mod, __VA_ARGS__)
#define KERCHUNK_LOG_I(mod, ...) kerchunk_log_msg(KERCHUNK_LOG_INFO, mod, __VA_ARGS__)
#define KERCHUNK_LOG_D(mod, ...) kerchunk_log_msg(KERCHUNK_LOG_DEBUG, mod, __VA_ARGS__)

#endif /* KERCHUNK_LOG_H */
