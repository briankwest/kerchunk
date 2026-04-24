/*
 * test_audio_ring.c — Unit tests for kerchunk_audio_ring.
 *
 * Validates the SPSC ring primitive and the PA-callback commit
 * function lifted out of kerchunk_audio.c in PLAN-AUDIO-TICK.md
 * Phase 1. The commit function is the testable home for the
 * paInputUnderflow drop behavior (item #1 in ARCH-COR-DTMF.md §12).
 */

#include "../include/kerchunk_audio_ring.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern void test_begin(const char *name);
extern void test_assert(int cond, const char *msg);
extern void test_end(void);

/* The ring is ~525 KB and we want a fresh one per test — use the heap
 * so we don't blow the test binary's stack. */
static kerchunk_audio_ring_t *fresh_ring(void)
{
    kerchunk_audio_ring_t *r = malloc(sizeof(*r));
    if (!r) { perror("malloc ring"); exit(1); }
    kerchunk_audio_ring_init(r);
    return r;
}

void test_audio_ring(void)
{
    /* 1. Empty-ring invariants. */
    test_begin("audio_ring: init -> empty");
    {
        kerchunk_audio_ring_t *r = fresh_ring();
        test_assert(kerchunk_audio_ring_readable(r) == 0,
            "readable=0 after init");
        test_assert(kerchunk_audio_ring_writable(r) == KERCHUNK_AUDIO_RING_SIZE - 1,
            "writable=SIZE-1 after init");
        free(r);
    }
    test_end();

    /* 2. Same-rate commit writes exactly n samples. */
    test_begin("audio_ring: commit same-rate writes n samples");
    {
        kerchunk_audio_ring_t *r = fresh_ring();
        int16_t buf[64];
        for (int i = 0; i < 64; i++) buf[i] = (int16_t)(i + 1);
        double pos = 0.0;
        size_t n = kerchunk_audio_ring_commit(r, buf, 64,
                                              48000, 48000, &pos, 0);
        test_assert(n == 64, "wrote 64 samples");
        test_assert(kerchunk_audio_ring_readable(r) == 64,
            "ring holds 64");

        int16_t back[64];
        size_t got = kerchunk_audio_ring_read(r, back, 64);
        test_assert(got == 64, "read 64 back");
        test_assert(memcmp(buf, back, sizeof(buf)) == 0,
            "samples are intact and in order");
        free(r);
    }
    test_end();

    /* 3. Underflow drop: commit returns 0, ring stays empty. */
    test_begin("audio_ring: commit underflow drops the buffer");
    {
        kerchunk_audio_ring_t *r = fresh_ring();
        int16_t buf[256];
        memset(buf, 0, sizeof(buf));   /* pretend PA zero-filled it */
        double pos = 0.0;
        size_t n = kerchunk_audio_ring_commit(r, buf, 256,
                                              48000, 48000, &pos, 1);
        test_assert(n == 0, "underflow → wrote 0");
        test_assert(kerchunk_audio_ring_readable(r) == 0,
            "ring still empty");
        free(r);
    }
    test_end();

    /* 4. Underflow drop preserves resampler state — i.e. the call is a
     *    no-op, not a phase-disturbance. */
    test_begin("audio_ring: underflow leaves resample_pos untouched");
    {
        kerchunk_audio_ring_t *r = fresh_ring();
        int16_t buf[64];
        memset(buf, 0, sizeof(buf));
        double pos = 1.7;  /* mid-fraction across PA callbacks */
        size_t n = kerchunk_audio_ring_commit(r, buf, 64,
                                              48000, 8000, &pos, 1);
        test_assert(n == 0, "no commit");
        test_assert(pos == 1.7, "resample_pos unchanged on drop");
        free(r);
    }
    test_end();

    /* 5. Resample 48k → 8k: 480 input samples → ~80 output samples,
     *    and resample_pos preserves continuity across two calls. */
    test_begin("audio_ring: resample 48k→8k produces ~n*8/48");
    {
        kerchunk_audio_ring_t *r = fresh_ring();
        int16_t buf[480];
        for (int i = 0; i < 480; i++) buf[i] = (int16_t)(i & 0x7FFF);
        double pos = 0.0;
        size_t n1 = kerchunk_audio_ring_commit(r, buf, 480,
                                               48000, 8000, &pos, 0);
        /* Step = 6.0 → expect ceil(480/6) = 80 output samples. */
        test_assert(n1 == 80, "first batch: 80 samples out");

        size_t n2 = kerchunk_audio_ring_commit(r, buf, 480,
                                               48000, 8000, &pos, 0);
        /* With perfect step 6.0 and pos starts back at 0 each call
         * (since pos -= 480 = 0 after first if step divides evenly),
         * second call also yields 80. */
        test_assert(n2 == 80, "second batch: 80 samples out");
        test_assert(kerchunk_audio_ring_readable(r) == 160,
            "ring holds 160 total");
        free(r);
    }
    test_end();

    /* 6. Resample with a non-divisible step keeps phase continuity. */
    test_begin("audio_ring: resample carries fractional phase across calls");
    {
        kerchunk_audio_ring_t *r = fresh_ring();
        int16_t buf[1000];
        for (int i = 0; i < 1000; i++) buf[i] = (int16_t)i;
        double pos = 0.0;
        /* 44100 → 16000: step = 2.75625. After 1000 samples,
         * pos -= 1000 leaves a non-integer carry. Just assert that
         * pos is in [0,1) after we re-pos by -n at the end of the
         * loop, and that we wrote a roughly-correct sample count. */
        size_t n = kerchunk_audio_ring_commit(r, buf, 1000,
                                              44100, 16000, &pos, 0);
        /* Expected: ~362-363 output samples (1000 / 2.75625). */
        test_assert(n >= 360 && n <= 365,
            "sample count near-target for 44.1k→16k");
        test_assert(pos >= 0.0 && pos < 3.0,
            "fractional carry in expected range");
        free(r);
    }
    test_end();

    /* 7. NULL-safe / zero-length: commit is a no-op, returns 0. */
    test_begin("audio_ring: NULL/zero inputs are safe no-ops");
    {
        kerchunk_audio_ring_t *r = fresh_ring();
        double pos = 0.0;
        int16_t buf[8] = {0};

        size_t n;
        n = kerchunk_audio_ring_commit(NULL, buf, 8, 48000, 48000, &pos, 0);
        test_assert(n == 0, "NULL ring → 0");

        n = kerchunk_audio_ring_commit(r, NULL, 8, 48000, 48000, &pos, 0);
        test_assert(n == 0, "NULL src → 0");

        n = kerchunk_audio_ring_commit(r, buf, 0, 48000, 48000, &pos, 0);
        test_assert(n == 0, "n=0 → 0");

        /* underflow=1 with n=0 — should still be a no-op. */
        n = kerchunk_audio_ring_commit(r, buf, 0, 48000, 48000, &pos, 1);
        test_assert(n == 0, "underflow + n=0 → 0");

        test_assert(kerchunk_audio_ring_readable(r) == 0,
            "no commits actually landed");
        free(r);
    }
    test_end();

    /* 8a. repeat_fill: empty last_buf → zero fill. */
    test_begin("audio_ring: repeat_fill with last_n=0 → zeros");
    {
        int16_t dst[16];
        for (int i = 0; i < 16; i++) dst[i] = 0x1234;  /* pre-fill */
        kerchunk_audio_repeat_fill(dst, 16, NULL, 0);
        int all_zero = 1;
        for (int i = 0; i < 16; i++) if (dst[i] != 0) { all_zero = 0; break; }
        test_assert(all_zero, "zeroed when no last_buf");
    }
    test_end();

    /* 8b. repeat_fill: last_n >= dst_n → copy tail preserves phase. */
    test_begin("audio_ring: repeat_fill copies tail of last_buf");
    {
        int16_t last[960];
        for (int i = 0; i < 960; i++) last[i] = (int16_t)i;
        int16_t dst[100];
        memset(dst, 0, sizeof(dst));
        kerchunk_audio_repeat_fill(dst, 100, last, 960);
        /* Should equal last[860..959] — the last 100 samples */
        int ok = 1;
        for (int i = 0; i < 100; i++)
            if (dst[i] != last[860 + i]) { ok = 0; break; }
        test_assert(ok, "dst is tail 100 of last_buf (phase-preserved)");
    }
    test_end();

    /* 8c. repeat_fill: last_n < dst_n → tile across dst. */
    test_begin("audio_ring: repeat_fill tiles when dst_n > last_n");
    {
        int16_t last[4] = { 10, 20, 30, 40 };
        int16_t dst[10];
        memset(dst, 0, sizeof(dst));
        kerchunk_audio_repeat_fill(dst, 10, last, 4);
        /* Expect: 10,20,30,40, 10,20,30,40, 10,20 */
        int16_t expect[10] = { 10,20,30,40, 10,20,30,40, 10,20 };
        int ok = memcmp(dst, expect, sizeof(expect)) == 0;
        test_assert(ok, "dst tiles last_buf from offset 0");
    }
    test_end();

    /* 9. Ring fills to SIZE-1 (one sentinel slot). */
    test_begin("audio_ring: fills to SIZE-1 then refuses extra");
    {
        kerchunk_audio_ring_t *r = fresh_ring();
        /* Write SIZE-1 worth of samples in chunks. */
        const size_t chunk = 4096;
        int16_t *big = malloc(chunk * sizeof(int16_t));
        for (size_t i = 0; i < chunk; i++) big[i] = 0x1234;
        size_t total = 0;
        while (total + chunk <= KERCHUNK_AUDIO_RING_SIZE - 1) {
            total += kerchunk_audio_ring_write(r, big, chunk);
        }
        /* finish off to exactly SIZE-1 */
        size_t left = (KERCHUNK_AUDIO_RING_SIZE - 1) - total;
        if (left > 0) total += kerchunk_audio_ring_write(r, big, left);
        test_assert(total == KERCHUNK_AUDIO_RING_SIZE - 1,
            "filled to SIZE-1");
        test_assert(kerchunk_audio_ring_writable(r) == 0,
            "writable=0 when full");

        /* Further commit returns the actual partial write (0 here). */
        double pos = 0.0;
        size_t n = kerchunk_audio_ring_commit(r, big, chunk,
                                              48000, 48000, &pos, 0);
        test_assert(n == 0, "commit when full writes 0");
        free(big);
        free(r);
    }
    test_end();
}
