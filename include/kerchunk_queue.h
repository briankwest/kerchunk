/*
 * kerchunk_queue.h — Outbound audio queue
 */

#ifndef KERCHUNK_QUEUE_H
#define KERCHUNK_QUEUE_H

#include <stdint.h>
#include <stddef.h>

/* Queue item types */
typedef enum {
    QUEUE_AUDIO_FILE,
    QUEUE_AUDIO_BUFFER,
    QUEUE_TONE,
    QUEUE_SILENCE,
} kerchunk_queue_item_type_t;

/* Queue item */
typedef struct kerchunk_queue_item {
    kerchunk_queue_item_type_t type;
    int priority;
    int id;
    union {
        struct { char path[256]; }                         file;
        struct { int16_t *buf; size_t n; int owns; }       buffer;
        struct { int freq; int duration_ms; int16_t amp; } tone;
        struct { int duration_ms; }                        silence;
    };
    struct kerchunk_queue_item *next;
} kerchunk_queue_item_t;

/* Set the queue's internal sample rate (must be called before use) */
void kerchunk_queue_set_rate(int sample_rate);

/* Queue API */
int  kerchunk_queue_init(void);
void kerchunk_queue_shutdown(void);

int  kerchunk_queue_add_file(const char *path, int priority);
int  kerchunk_queue_add_buffer(const int16_t *buf, size_t n, int priority);
int  kerchunk_queue_add_tone(int freq_hz, int duration_ms, int16_t amplitude, int priority);
int  kerchunk_queue_add_silence(int duration_ms, int priority);

int  kerchunk_queue_flush(void);
int  kerchunk_queue_depth(void);

/*
 * Drain the next chunk of audio from the queue.
 * out:     buffer to fill with samples
 * max_n:   max samples to write
 * Returns: number of samples written, 0 if queue is empty/done
 */
int  kerchunk_queue_drain(int16_t *out, size_t max_n);

/* Check if queue is currently draining (mid-item) */
int  kerchunk_queue_is_draining(void);

#endif /* KERCHUNK_QUEUE_H */
