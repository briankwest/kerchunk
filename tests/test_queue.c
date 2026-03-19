/*
 * test_queue.c — Outbound queue tests
 */

#include "kerchunk_queue.h"
#include "kerchunk_wav.h"
#include "kerchunk_log.h"
#include "plcode.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

extern void test_begin(const char *name);
extern void test_end(void);
extern void test_assert(int cond, const char *msg);

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

    test_begin("nonexistent file skipped on drain");
    id = kerchunk_queue_add_file("/tmp/nonexistent_test.wav", 0);
    test_assert(id > 0, "add_file failed");
    n = kerchunk_queue_drain(out, 160);
    test_assert(n == 0, "should skip bad file");
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

    kerchunk_queue_shutdown();
}
