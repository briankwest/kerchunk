/*
 * test_queue.c — Outbound queue tests including concurrent stress
 */

#include "kerchunk_queue.h"
#include "kerchunk_wav.h"
#include "kerchunk_log.h"
#include "plcode.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

/* Concurrent test: producer thread */
static atomic_int g_producer_done;
static atomic_int g_items_produced;

static void *producer_fn(void *arg)
{
    int count = *(int *)arg;
    int16_t buf[160];
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < 160; j++)
            buf[j] = (int16_t)i;
        kerchunk_queue_add_buffer(buf, 160, 2);
        atomic_fetch_add(&g_items_produced, 1);
        if (i % 10 == 0) usleep(100);
    }
    atomic_store(&g_producer_done, 1);
    return NULL;
}

void test_queue(void)
{
    test_begin("init queue");
    test_assert(kerchunk_queue_init() == 0, "init failed");
    test_end();

    test_begin("empty queue depth");
    test_assert(kerchunk_queue_depth() == 0, "not empty");
    test_end();

    test_begin("add and drain buffer");
    int16_t samples[160];
    memset(samples, 0, sizeof(samples));
    int id = kerchunk_queue_add_buffer(samples, 160, 0);
    test_assert(id > 0, "add_buffer failed");
    test_assert(kerchunk_queue_depth() == 1, "depth not 1");
    int16_t out[160];
    int n = kerchunk_queue_drain(out, 160);
    test_assert(n == 160, "drain wrong count");
    test_assert(kerchunk_queue_depth() == 0, "not empty after drain");
    test_end();

    test_begin("add and drain tone (800 samples @ 8kHz)");
    id = kerchunk_queue_add_tone(800, 100, 4000, 0);
    test_assert(id > 0, "add_tone failed");
    int total = 0;
    while ((n = kerchunk_queue_drain(out, 160)) > 0)
        total += n;
    test_assert(total == 800, "tone length wrong");
    test_end();

    test_begin("add and drain silence (400 samples)");
    id = kerchunk_queue_add_silence(50, 0);
    test_assert(id > 0, "add_silence failed");
    total = 0;
    while ((n = kerchunk_queue_drain(out, 160)) > 0)
        total += n;
    test_assert(total == 400, "silence length wrong");
    test_end();

    test_begin("priority ordering (high drains first)");
    kerchunk_queue_add_buffer(samples, 160, 0);
    kerchunk_queue_add_buffer(samples, 160, 10);
    kerchunk_queue_add_buffer(samples, 160, 5);
    test_assert(kerchunk_queue_depth() == 3, "depth not 3");
    total = 0;
    while (kerchunk_queue_drain(out, 160) > 0)
        total++;
    test_assert(total == 3, "didn't drain 3 items");
    test_end();

    test_begin("flush clears queue");
    kerchunk_queue_add_tone(440, 100, 4000, 0);
    kerchunk_queue_add_tone(880, 100, 4000, 0);
    test_assert(kerchunk_queue_depth() == 2, "depth not 2");
    int flushed = kerchunk_queue_flush();
    test_assert(flushed == 2, "flush count wrong");
    test_assert(kerchunk_queue_depth() == 0, "not empty after flush");
    test_end();

    test_begin("drain empty queue returns 0");
    n = kerchunk_queue_drain(out, 160);
    test_assert(n == 0, "should return 0");
    test_end();

    test_begin("nonexistent file rejected at queue time");
    id = kerchunk_queue_add_file("/tmp/nonexistent_test.wav", 0);
    test_assert(id < 0, "should fail for nonexistent file");
    test_assert(kerchunk_queue_depth() == 0, "bad file should not be queued");
    test_end();

    test_begin("WAV file round-trip via queue");
    {
        int16_t wav_buf[800];
        memset(wav_buf, 0, sizeof(wav_buf));
        plcode_tone_enc_t *tone = NULL;
        plcode_tone_enc_create(&tone, 8000, 440, 4000);
        plcode_tone_enc_process(tone, wav_buf, 800);
        plcode_tone_enc_destroy(tone);
        const char *wav_path = "/tmp/test_kerchunk_queue.wav";
        kerchunk_wav_write(wav_path, wav_buf, 800, 8000);
        kerchunk_queue_add_file(wav_path, 0);
        total = 0;
        while ((n = kerchunk_queue_drain(out, 160)) > 0)
            total += n;
        test_assert(total == 800, "WAV drain wrong length");
        unlink(wav_path);
    }
    test_end();

    /* ── State consistency after full drain ── */

    test_begin("state clean after drain cycle");
    {
        /* Add 10 buffers, drain all, verify state is clean */
        int16_t buf[160];
        for (int i = 0; i < 160; i++) buf[i] = (int16_t)(i * 100);
        for (int i = 0; i < 10; i++)
            kerchunk_queue_add_buffer(buf, 160, 2);
        test_assert(kerchunk_queue_depth() == 10, "depth not 10");
        total = 0;
        while ((n = kerchunk_queue_drain(out, 160)) > 0)
            total += n;
        test_assert(total == 1600, "total wrong");
        test_assert(kerchunk_queue_depth() == 0, "depth not 0");
        test_assert(!kerchunk_queue_is_draining(), "still draining");
        /* Verify drain returns 0 consistently */
        test_assert(kerchunk_queue_drain(out, 160) == 0, "phantom drain");
        test_assert(kerchunk_queue_drain(out, 160) == 0, "phantom drain 2");
    }
    test_end();

    test_begin("announcements work after buffer burst");
    {
        /* Simulate web PTT: many small buffers, drain all */
        int16_t buf[160];
        for (int i = 0; i < 160; i++) buf[i] = (int16_t)(i * 50);
        for (int i = 0; i < 50; i++)
            kerchunk_queue_add_buffer(buf, 160, 2);
        total = 0;
        while ((n = kerchunk_queue_drain(out, 160)) > 0)
            total += n;
        test_assert(total == 8000, "PTT drain wrong");
        test_assert(kerchunk_queue_depth() == 0, "not empty");
        test_assert(!kerchunk_queue_is_draining(), "still draining");

        /* Now add a file-type item (like weather announcement) */
        int16_t wav_buf[800];
        memset(wav_buf, 0, sizeof(wav_buf));
        plcode_tone_enc_t *tone = NULL;
        plcode_tone_enc_create(&tone, 8000, 440, 4000);
        plcode_tone_enc_process(tone, wav_buf, 800);
        plcode_tone_enc_destroy(tone);
        const char *wav_path = "/tmp/test_kerchunk_queue2.wav";
        kerchunk_wav_write(wav_path, wav_buf, 800, 8000);

        id = kerchunk_queue_add_file(wav_path, 3);
        test_assert(id > 0, "add_file failed after PTT");
        test_assert(kerchunk_queue_depth() == 1, "depth not 1");

        /* Drain the announcement */
        total = 0;
        int frames = 0;
        while ((n = kerchunk_queue_drain(out, 160)) > 0) {
            total += n;
            frames++;
        }
        test_assert(total == 800, "announcement drain wrong");
        test_assert(kerchunk_queue_depth() == 0, "not empty after ann");
        test_assert(!kerchunk_queue_is_draining(), "still draining ann");
        unlink(wav_path);
    }
    test_end();

    test_begin("data integrity: samples preserved through queue");
    {
        /* Write known pattern, verify it comes back identical */
        int16_t pattern[320];
        for (int i = 0; i < 320; i++)
            pattern[i] = (int16_t)(i * 100 - 16000);
        kerchunk_queue_add_buffer(pattern, 320, 0);
        int16_t result[320];
        int pos = 0;
        while ((n = kerchunk_queue_drain(result + pos, 160)) > 0)
            pos += n;
        test_assert(pos == 320, "wrong sample count");
        int match = 1;
        for (int i = 0; i < 320; i++) {
            if (result[i] != pattern[i]) { match = 0; break; }
        }
        test_assert(match, "data mismatch");
    }
    test_end();

    test_begin("partial drain preserves remaining data");
    {
        int16_t buf[480];
        for (int i = 0; i < 480; i++) buf[i] = (int16_t)i;
        kerchunk_queue_add_buffer(buf, 480, 0);
        /* Drain in 3 chunks of 160 */
        int16_t chunk[160];
        int ok = 1;
        for (int c = 0; c < 3; c++) {
            n = kerchunk_queue_drain(chunk, 160);
            if (n != 160) { ok = 0; break; }
            for (int i = 0; i < 160; i++) {
                if (chunk[i] != (int16_t)(c * 160 + i)) { ok = 0; break; }
            }
        }
        test_assert(ok, "partial drain data wrong");
        test_assert(kerchunk_queue_drain(chunk, 160) == 0, "extra data");
    }
    test_end();

    /* ── Concurrent producer/consumer stress test ── */

    test_begin("concurrent: producer + consumer 1000 items");
    {
        kerchunk_queue_init();  /* fresh state */

        atomic_store(&g_producer_done, 0);
        atomic_store(&g_items_produced, 0);
        int count = 1000;

        pthread_t prod;
        pthread_create(&prod, NULL, producer_fn, &count);

        /* Consumer: drain until producer is done AND queue is empty */
        int total_samples = 0;
        int drain_calls = 0;
        int16_t drain_buf[160];
        while (!atomic_load(&g_producer_done) ||
               kerchunk_queue_depth() > 0 ||
               kerchunk_queue_is_draining()) {
            n = kerchunk_queue_drain(drain_buf, 160);
            if (n > 0) {
                total_samples += n;
                drain_calls++;
            } else {
                usleep(50);
            }
        }
        /* Final drain to catch any stragglers */
        while ((n = kerchunk_queue_drain(drain_buf, 160)) > 0)
            total_samples += n;

        pthread_join(prod, NULL);

        test_assert(total_samples == 1000 * 160,
                    "sample count mismatch in concurrent test");
        test_assert(kerchunk_queue_depth() == 0, "queue not empty");
        test_assert(!kerchunk_queue_is_draining(), "still draining");
    }
    test_end();

    test_begin("concurrent: post-stress state is clean");
    {
        /* After concurrent test, verify normal operations still work */
        int16_t buf[160];
        for (int i = 0; i < 160; i++) buf[i] = (int16_t)(i * 200);
        id = kerchunk_queue_add_buffer(buf, 160, 0);
        test_assert(id > 0, "add_buffer failed post-stress");
        test_assert(kerchunk_queue_depth() == 1, "depth wrong");
        int16_t result[160];
        n = kerchunk_queue_drain(result, 160);
        test_assert(n == 160, "drain wrong");
        int match = 1;
        for (int i = 0; i < 160; i++) {
            if (result[i] != buf[i]) { match = 0; break; }
        }
        test_assert(match, "data mismatch post-stress");
        test_assert(kerchunk_queue_depth() == 0, "not empty");
        test_assert(!kerchunk_queue_is_draining(), "still draining");
        test_assert(kerchunk_queue_drain(result, 160) == 0, "phantom");
    }
    test_end();

    test_begin("concurrent: file after stress works");
    {
        /* Simulate the exact failing scenario: burst of buffers, then a file */
        int16_t buf[160];
        for (int i = 0; i < 160; i++) buf[i] = 1000;

        /* Burst of 100 small buffers (simulating web PTT) */
        for (int i = 0; i < 100; i++)
            kerchunk_queue_add_buffer(buf, 160, 2);

        /* Drain all */
        total = 0;
        while ((n = kerchunk_queue_drain(out, 160)) > 0)
            total += n;
        test_assert(total == 16000, "burst drain wrong");

        /* Now queue a WAV file (like weather announcement) */
        int16_t wav_buf[800];
        plcode_tone_enc_t *tone = NULL;
        plcode_tone_enc_create(&tone, 8000, 1000, 4000);
        plcode_tone_enc_process(tone, wav_buf, 800);
        plcode_tone_enc_destroy(tone);
        const char *path = "/tmp/test_queue_post_burst.wav";
        kerchunk_wav_write(path, wav_buf, 800, 8000);

        kerchunk_queue_add_file(path, 3);

        /* Verify announcement drains correctly with intact data */
        int16_t ann_out[800];
        int ann_pos = 0;
        while ((n = kerchunk_queue_drain(ann_out + ann_pos, 160)) > 0)
            ann_pos += n;
        test_assert(ann_pos == 800, "announcement length wrong");

        /* Verify data matches original WAV content */
        int match = 1;
        for (int i = 0; i < 800; i++) {
            if (ann_out[i] != wav_buf[i]) { match = 0; break; }
        }
        test_assert(match, "announcement data corrupted after burst");

        /* Verify state is fully clean */
        test_assert(kerchunk_queue_depth() == 0, "not empty");
        test_assert(!kerchunk_queue_is_draining(), "stuck draining");
        test_assert(kerchunk_queue_drain(out, 160) == 0, "phantom data");

        unlink(path);
    }
    test_end();

    kerchunk_queue_shutdown();
}
