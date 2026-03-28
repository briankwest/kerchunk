/*
 * kerchunk_console.h — Embedded interactive CLI console for foreground mode
 */

#ifndef KERCHUNK_CONSOLE_H
#define KERCHUNK_CONSOLE_H

int  kerchunk_console_init(void);
void kerchunk_console_shutdown(void);
void kerchunk_console_log_line(const char *line);

#endif /* KERCHUNK_CONSOLE_H */
