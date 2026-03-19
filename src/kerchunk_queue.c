/*
 * kerchunk_queue.c — Outbound priority queue + drain logic
 *
 * Thread safety: a single mutex protects all queue state.  Producers
 * (any thread) do heavy work (malloc, memcpy, disk I/O) outside the
 * lock and only hold it for the linked-list insert.  The drain path
 * (audio thread, 50 Hz) holds the lock for pointer ops + a small
 * memcpy — well under 1 µs.
 */

#include "kerchunk_queue.h"
#include "kerchunk_wav.h"
#include "kerchunk_log.h"
#include "plcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define LOG_MOD  "queue"
#define RATE     8000

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static kerchunk_queue_item_t *g_head;
static int  g_next_id = 1;
static int  g_count;

/* Current drain state */
static int16_t *g_drain_buf;
static size_t   g_drain_len;
static size_t   g_drain_pos;
static int      g_draining;
static int      g_drain_item_id;
static kerchunk_queue_item_type_t g_drain_type;

/* 1 while queue is actively playing a batch — prevents preemption */
static int      g_batch_active;

/* Inter-item gap: silence inserted between consecutive file items */
#define GAP_MS   10
#define GAP_SAMPLES ((size_t)(RATE * GAP_MS) / 1000)
static size_t   g_gap_remaining;

int kerchunk_queue_init(void)
{
    pthread_mutex_lock(&g_mutex);
    g_head    = NULL;
    g_next_id = 1;
    g_count   = 0;
    g_drain_buf = NULL;
    g_drain_len = 0;
    g_drain_pos = 0;
    g_draining  = 0;
    g_batch_active = 0;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static void free_item(kerchunk_queue_item_t *item)
{
    if (item->type == QUEUE_AUDIO_BUFFER && item->buffer.owns)
        free(item->buffer.buf);
    free(item);
}

void kerchunk_queue_shutdown(void)
{
    pthread_mutex_lock(&g_mutex);
    /* flush inline */
    while (g_head) {
        kerchunk_queue_item_t *next = g_head->next;
        free_item(g_head);
        g_head = next;
    }
    g_count = 0;
    free(g_drain_buf);
    g_drain_buf = NULL;
    g_drain_len = 0;
    g_drain_pos = 0;
    g_draining  = 0;
    g_batch_active = 0;
    pthread_mutex_unlock(&g_mutex);
}

/* Insert item into queue (caller must hold g_mutex).
 * While a batch is playing, new items append to tail (no preemption).
 * When idle, items are inserted in priority order. */
static void insert_item(kerchunk_queue_item_t *item)
{
    if (g_batch_active || g_draining || g_gap_remaining > 0) {
        /* Queue is mid-playback — append to tail */
        kerchunk_queue_item_t **tail = &g_head;
        while (*tail)
            tail = &(*tail)->next;
        *tail = item;
        item->next = NULL;
    } else if (!g_head || item->priority > g_head->priority) {
        item->next = g_head;
        g_head = item;
    } else {
        kerchunk_queue_item_t *cur = g_head;
        while (cur->next && cur->next->priority >= item->priority)
            cur = cur->next;
        item->next = cur->next;
        cur->next = item;
    }
    g_count++;
}

int kerchunk_queue_add_file(const char *path, int priority)
{
    if (!path)
        return -1;

    /* Pre-read file into memory OUTSIDE the lock — disk I/O must not
     * hold the mutex. */
    int16_t *buf = NULL;
    size_t n = 0;
    int rate = 0;

    if (kerchunk_wav_read(path, &buf, &n, &rate) != 0) {
        if (kerchunk_pcm_read(path, &buf, &n) != 0) {
            KERCHUNK_LOG_E(LOG_MOD, "failed to read audio file: %s", path);
            return -1;
        }
    }

    kerchunk_queue_item_t *item = calloc(1, sizeof(*item));
    if (!item) {
        free(buf);
        return -1;
    }

    item->type       = QUEUE_AUDIO_FILE;
    item->priority   = priority;
    item->buffer.buf = buf;
    item->buffer.n   = n;
    item->buffer.owns = 1;

    pthread_mutex_lock(&g_mutex);
    item->id = g_next_id++;
    int id = item->id;
    insert_item(item);
    pthread_mutex_unlock(&g_mutex);
    /* item may be freed by drain thread after unlock — use saved id */

    KERCHUNK_LOG_D(LOG_MOD, "queued file: %s (%zu samples, id=%d, pri=%d)",
                   path, n, id, priority);
    return id;
}

int kerchunk_queue_add_buffer(const int16_t *buf, size_t n, int priority)
{
    if (!buf || n == 0)
        return -1;

    /* Allocate and copy OUTSIDE the lock */
    kerchunk_queue_item_t *item = calloc(1, sizeof(*item));
    if (!item)
        return -1;

    int16_t *copy = malloc(n * sizeof(int16_t));
    if (!copy) {
        free(item);
        return -1;
    }
    memcpy(copy, buf, n * sizeof(int16_t));

    item->type       = QUEUE_AUDIO_BUFFER;
    item->priority   = priority;
    item->buffer.buf = copy;
    item->buffer.n   = n;
    item->buffer.owns = 1;

    pthread_mutex_lock(&g_mutex);
    item->id = g_next_id++;
    int id = item->id;
    insert_item(item);
    pthread_mutex_unlock(&g_mutex);

    KERCHUNK_LOG_D(LOG_MOD, "queued buffer: %zu samples (id=%d, pri=%d)", n, id, priority);
    return id;
}

int kerchunk_queue_add_tone(int freq_hz, int duration_ms, int16_t amplitude, int priority)
{
    if (freq_hz <= 0 || duration_ms <= 0 || duration_ms > 300000)
        return -1;

    /* Pre-render tone OUTSIDE the lock */
    size_t n = ((size_t)RATE * (size_t)duration_ms) / 1000;
    int16_t *buf = calloc(n, sizeof(int16_t));
    if (!buf)
        return -1;

    plcode_tone_enc_t *tone = NULL;
    if (plcode_tone_enc_create(&tone, RATE, freq_hz, amplitude) == PLCODE_OK) {
        plcode_tone_enc_process(tone, buf, n);
        plcode_tone_enc_destroy(tone);
    }

    kerchunk_queue_item_t *item = calloc(1, sizeof(*item));
    if (!item) {
        free(buf);
        return -1;
    }

    item->type       = QUEUE_AUDIO_BUFFER;
    item->priority   = priority;
    item->buffer.buf = buf;
    item->buffer.n   = n;
    item->buffer.owns = 1;

    pthread_mutex_lock(&g_mutex);
    item->id = g_next_id++;
    int id = item->id;
    insert_item(item);
    pthread_mutex_unlock(&g_mutex);

    KERCHUNK_LOG_D(LOG_MOD, "queued tone: %d Hz, %d ms (id=%d)", freq_hz, duration_ms, id);
    return id;
}

int kerchunk_queue_add_silence(int duration_ms, int priority)
{
    if (duration_ms <= 0 || duration_ms > 300000)
        return -1;

    size_t n = ((size_t)RATE * (size_t)duration_ms) / 1000;
    int16_t *buf = calloc(n, sizeof(int16_t));
    if (!buf)
        return -1;

    kerchunk_queue_item_t *item = calloc(1, sizeof(*item));
    if (!item) {
        free(buf);
        return -1;
    }

    item->type       = QUEUE_AUDIO_BUFFER;
    item->priority   = priority;
    item->buffer.buf = buf;
    item->buffer.n   = n;
    item->buffer.owns = 1;

    pthread_mutex_lock(&g_mutex);
    item->id = g_next_id++;
    int id = item->id;
    insert_item(item);
    pthread_mutex_unlock(&g_mutex);

    KERCHUNK_LOG_D(LOG_MOD, "queued silence: %d ms (id=%d)", duration_ms, id);
    return id;
}

int kerchunk_queue_flush(void)
{
    pthread_mutex_lock(&g_mutex);
    int flushed = 0;
    while (g_head) {
        kerchunk_queue_item_t *next = g_head->next;
        free_item(g_head);
        g_head = next;
        flushed++;
    }
    g_count = 0;

    /* Also reset drain state */
    free(g_drain_buf);
    g_drain_buf = NULL;
    g_drain_len = 0;
    g_drain_pos = 0;
    g_draining  = 0;
    g_batch_active = 0;
    pthread_mutex_unlock(&g_mutex);

    return flushed;
}

int kerchunk_queue_depth(void)
{
    pthread_mutex_lock(&g_mutex);
    int d = g_count;
    pthread_mutex_unlock(&g_mutex);
    return d;
}

/* Load the next item for draining (caller must hold g_mutex) */
static int load_next_item(void)
{
    while (g_head) {
        /* Pop head */
        kerchunk_queue_item_t *item = g_head;
        g_head = item->next;
        g_count--;

        free(g_drain_buf);
        g_drain_buf = NULL;
        g_drain_len = 0;
        g_drain_pos = 0;

        if (item->type == QUEUE_AUDIO_FILE) {
            /* File data was pre-read into buffer at queue time */
            if (item->buffer.buf && item->buffer.n > 0) {
                g_drain_buf   = item->buffer.buf;
                g_drain_len   = item->buffer.n;
                item->buffer.owns = 0;  /* drain owns it now */
            } else {
                KERCHUNK_LOG_E(LOG_MOD, "file item has no data (id=%d)", item->id);
                free_item(item);
                continue;
            }
        } else if (item->type == QUEUE_AUDIO_BUFFER) {
            if (item->buffer.owns) {
                g_drain_buf   = item->buffer.buf;
                g_drain_len   = item->buffer.n;
                item->buffer.owns = 0;
            } else {
                g_drain_buf = malloc(item->buffer.n * sizeof(int16_t));
                if (!g_drain_buf) {
                    free_item(item);
                    continue;  /* Skip on alloc failure */
                }
                memcpy(g_drain_buf, item->buffer.buf, item->buffer.n * sizeof(int16_t));
                g_drain_len = item->buffer.n;
            }
        }

        g_drain_item_id = item->id;
        g_drain_type    = item->type;
        g_draining = 1;
        g_batch_active = 1;
        free_item(item);
        return 1;
    }
    g_batch_active = 0;
    return 0;
}

int kerchunk_queue_drain(int16_t *out, size_t max_n)
{
    if (!out || max_n == 0)
        return 0;

    pthread_mutex_lock(&g_mutex);

    /* Emit inter-item silence gap before loading next item */
    if (g_gap_remaining > 0) {
        size_t gap_n = g_gap_remaining < max_n ? g_gap_remaining : max_n;
        memset(out, 0, gap_n * sizeof(int16_t));
        g_gap_remaining -= gap_n;
        pthread_mutex_unlock(&g_mutex);
        return (int)gap_n;
    }

    /* If not currently draining, load next item */
    if (!g_draining || g_drain_pos >= g_drain_len) {
        g_draining = 0;
        if (!load_next_item()) {
            pthread_mutex_unlock(&g_mutex);
            return 0;
        }
    }

    /* Copy samples from drain buffer */
    size_t remaining = g_drain_len - g_drain_pos;
    size_t to_copy = remaining < max_n ? remaining : max_n;
    memcpy(out, g_drain_buf + g_drain_pos, to_copy * sizeof(int16_t));
    g_drain_pos += to_copy;

    /* Check if item is complete */
    if (g_drain_pos >= g_drain_len) {
        g_draining = 0;
        free(g_drain_buf);
        g_drain_buf = NULL;
        /* Insert silence gap between file items for natural pacing */
        if (g_head && g_drain_type == QUEUE_AUDIO_FILE)
            g_gap_remaining = GAP_SAMPLES;
    }

    pthread_mutex_unlock(&g_mutex);
    return (int)to_copy;
}

int kerchunk_queue_is_draining(void)
{
    pthread_mutex_lock(&g_mutex);
    int d = g_draining;
    pthread_mutex_unlock(&g_mutex);
    return d;
}
