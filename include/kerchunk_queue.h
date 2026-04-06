/*
 * kerchunk_queue.h — Outbound audio queue
 */

#ifndef KERCHUNK_QUEUE_H
#define KERCHUNK_QUEUE_H

#include <stdint.h>
#include <stddef.h>

/* Queue item flags */
#define QUEUE_FLAG_NO_TAIL  0x01  /* suppress courtesy/tail tone after this item */

/* Max length of source tag for queue items */
#define QUEUE_SOURCE_MAX  16

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
    int flags;
    char source[QUEUE_SOURCE_MAX];  /* module tag: "cwid", "weather", "tts", etc. */
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
int  kerchunk_queue_add_buffer(const int16_t *buf, size_t n, int priority, int flags);
int  kerchunk_queue_add_tone(int freq_hz, int duration_ms, int16_t amplitude, int priority);
int  kerchunk_queue_add_silence(int duration_ms, int priority);

/* Source-tagged versions — same as above but set the source tag on the item */
int  kerchunk_queue_add_buffer_src(const int16_t *buf, size_t n, int priority,
                                    int flags, const char *source);
int  kerchunk_queue_add_tone_src(int freq_hz, int duration_ms, int16_t amplitude,
                                  int priority, const char *source);
int  kerchunk_queue_add_silence_src(int duration_ms, int priority, const char *source);

int  kerchunk_queue_flush(void);
int  kerchunk_queue_depth(void);

/* Tag a queued item with a source identifier (e.g. "cwid", "weather").
 * Must be called before the item starts draining. */
void kerchunk_queue_tag_item(int id, const char *source);

/* Get the source tag of the item currently being drained (or last drained).
 * Returns "" if no source was set. */
const char *kerchunk_queue_drain_source(void);

/* Flush the queue and return info about what was preempted.
 * Sets *out_source to the source tag of the item that was being drained
 * (or the first item flushed). Returns number of items flushed. */
int  kerchunk_queue_preempt(char *out_source, size_t source_sz);

/*
 * Drain the next chunk of audio from the queue.
 * out:     buffer to fill with samples
 * max_n:   max samples to write
 * Returns: number of samples written, 0 if queue is empty/done
 */
int  kerchunk_queue_drain(int16_t *out, size_t max_n);

/* Check if queue is currently draining (mid-item) */
int  kerchunk_queue_is_draining(void);

/* Get flags of the last drained item (valid after drain returns 0) */
int  kerchunk_queue_drain_flags(void);

#endif /* KERCHUNK_QUEUE_H */
