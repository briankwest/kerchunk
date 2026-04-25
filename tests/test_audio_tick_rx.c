/*
 * test_audio_tick_rx.c — Unit tests for kerchunk_audio_tick_rx.
 *
 * Covers the RX sub-tick lifted from audio_thread_fn. The two
 * critical items this test suite covers that previously had no
 * coverage:
 *   - decoder reset on COR-assert edge (ARCH-COR-DTMF.md §12 item #4)
 *   - DTMF_DIGIT / DTMF_END edge detection via prev_dtmf
 *
 * We use a real plcode DTMF decoder + encoder so tests exercise the
 * same code paths as the on-radio loop. No mocks.
 */

#include "../include/kerchunk_audio_tick.h"
#include "../include/kerchunk_audio_ring.h"   /* for KERCHUNK_AUDIO_RING_MASK */
#include "plcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void test_begin(const char *name);
extern void test_assert(int cond, const char *msg);
extern void test_end(void);

/* ── helpers ────────────────────────────────────────────────── */

/* Generate an audio buffer with a held DTMF digit via libplcode.
 * `samples` is filled with `n` int16 samples at `rate` Hz.
 * Buffer is zeroed first; encoder mixes tone into it. */
static void generate_dtmf(int16_t *samples, size_t n, int rate, char digit)
{
    memset(samples, 0, n * sizeof(int16_t));
    plcode_dtmf_enc_t *enc = NULL;
    plcode_dtmf_enc_create(&enc, rate, digit,
                           24000 /* amplitude — hot tone for fast latch */);
    plcode_dtmf_enc_process(enc, samples, n);
    plcode_dtmf_enc_destroy(enc);
}

/* Silence buffer */
static void gen_silence(int16_t *samples, size_t n)
{
    memset(samples, 0, n * sizeof(int16_t));
}

/* Low-level noise (below the 200-RMS relay early-stop floor). */
static void gen_low_noise(int16_t *samples, size_t n)
{
    for (size_t i = 0; i < n; i++)
        samples[i] = (i & 1) ? 50 : -50;
}

/* Mid-level audio above the noise floor. */
static void gen_speech_like(int16_t *samples, size_t n)
{
    for (size_t i = 0; i < n; i++)
        samples[i] = (int16_t)((i & 1) ? 2000 : -2000);
}

static plcode_dtmf_dec_t *new_decoder(int rate)
{
    plcode_dtmf_dec_t *d = NULL;
    plcode_dtmf_dec_opts_t opts = {0};        /* zero-init → library defaults */
    opts.hits_to_begin  = 1;   /* match kerchunk's runtime defaults */
    opts.misses_to_end  = 3;
    opts.min_off_frames = 1;
    plcode_dtmf_dec_create_ex(&d, rate, &opts);
    return d;
}

/* One-shot: init state + rx_state to a sane default, with relay mode on. */
static void init_state(kerchunk_audio_state_t *s,
                       kerchunk_audio_tick_rx_state_t *rx,
                       int software_relay)
{
    memset(s, 0, sizeof(*s));
    s->software_relay = software_relay;
    s->relay_drain_ms = 500;
    s->tx_tail_rem    = -1;
    memset(rx, 0, sizeof(*rx));
}

/* ── tests ──────────────────────────────────────────────────── */

void test_audio_tick_rx(void)
{
    const int rate = 48000;
    const int fs   = 960;  /* 20 ms at 48 kHz */

    /* 1. Decoder reset on COR-assert edge (ARCH-COR-DTMF §12 #4).
     *
     * Scenario: session ended mid-tone; decoder still holds '7'
     * state. User keys up and presses '5'. After the COR-assert
     * edge, the FIRST digit the tick surfaces must be '5' (the
     * reset dropped the stale Goertzel + hysteresis state).
     *
     * We generate long continuous tone buffers so the decoder sees
     * phase-continuous audio across multiple 20ms frames. */
    test_begin("tick_rx: COR-assert edge resets decoder");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 1);

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        const int tone_frames = 32;
        int16_t *tone5 = calloc(fs * tone_frames, sizeof(int16_t));
        int16_t *tone7 = calloc(fs * tone_frames, sizeof(int16_t));
        generate_dtmf(tone5, fs * tone_frames, rate, '5');
        generate_dtmf(tone7, fs * tone_frames, rate, '7');

        /* Contaminate the decoder: feed continuous '7' tone until
         * it latches, then leave state mid-tone. */
        plcode_dtmf_result_t res;
        for (int i = 0; i < tone_frames; i++)
            plcode_dtmf_dec_process(dec, tone7 + i * fs, fs, &res);
        rx.prev_dtmf = 1;  /* was tracking a tone */

        /* COR-assert edge → reset. Feed continuous '5' in 20ms frames.
         * First digit surfaced must be '5'. */
        kerchunk_audio_tick_rx_out_t out;
        char first_digit = '\0';
        for (int i = 0; i < tone_frames; i++) {
            kerchunk_audio_tick_rx(&s, &rx, dec,
                                   tone5 + i * fs, fs,
                                   1, 0, 0, rate, &out);
            if (out.event == KERCHUNK_TICK_RX_DTMF_DIGIT) {
                first_digit = out.digit;
                break;
            }
        }
        test_assert(first_digit == '5',
            "first surfaced digit after reset is '5', not stale '7'");

        plcode_dtmf_dec_destroy(dec);
        free(tone5);
        free(tone7);
    }
    test_end();

    /* 2. DTMF_DIGIT fires on rising edge with digit populated. */
    test_begin("tick_rx: DTMF_DIGIT on rising edge, carries digit");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 0);  /* no relay */

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        int16_t *tone = calloc(fs, sizeof(int16_t));
        generate_dtmf(tone, fs, rate, '3');

        /* Tick with relay_active=1 so decoder runs.
         * First rising-edge frame should emit DIGIT='3'. */
        kerchunk_audio_tick_rx_out_t out;
        int got_digit_event = 0;
        char got_digit = '\0';
        for (int i = 0; i < 8; i++) {
            kerchunk_audio_tick_rx(&s, &rx, dec,
                                   tone, fs,
                                   1, 0, 0, rate, &out);
            if (out.event == KERCHUNK_TICK_RX_DTMF_DIGIT) {
                got_digit_event = 1;
                got_digit = out.digit;
                break;
            }
        }
        test_assert(got_digit_event, "saw DTMF_DIGIT event");
        test_assert(got_digit == '3', "digit is '3'");

        plcode_dtmf_dec_destroy(dec);
        free(tone);
    }
    test_end();

    /* 3. DTMF_END fires when tone stops. */
    test_begin("tick_rx: DTMF_END fires when tone stops");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 0);

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        int16_t *tone = calloc(fs, sizeof(int16_t));
        int16_t *sil  = calloc(fs, sizeof(int16_t));
        generate_dtmf(tone, fs, rate, '9');
        gen_silence(sil, fs);

        kerchunk_audio_tick_rx_out_t out;
        /* Drive tone until we've seen DIGIT */
        int saw_digit = 0;
        for (int i = 0; i < 10 && !saw_digit; i++) {
            kerchunk_audio_tick_rx(&s, &rx, dec, tone, fs,
                                   1, 0, 0, rate, &out);
            if (out.event == KERCHUNK_TICK_RX_DTMF_DIGIT) saw_digit = 1;
        }
        test_assert(saw_digit, "saw DIGIT first");

        /* Then feed silence — eventually END must fire. */
        int saw_end = 0;
        for (int i = 0; i < 20 && !saw_end; i++) {
            kerchunk_audio_tick_rx(&s, &rx, dec, sil, fs,
                                   1, 0, 0, rate, &out);
            if (out.event == KERCHUNK_TICK_RX_DTMF_END) saw_end = 1;
        }
        test_assert(saw_end, "saw DTMF_END");
        test_assert(out.dtmf_active == 0, "dtmf_active dropped to 0");
        test_assert(rx.prev_dtmf == 0, "prev_dtmf cleared");

        plcode_dtmf_dec_destroy(dec);
        free(tone);
        free(sil);
    }
    test_end();

    /* 4. Decoder NOT run when !relay_active && drain=0. */
    test_begin("tick_rx: decoder skipped when COR=0 and drain=0");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 0);

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        int16_t *tone = calloc(fs, sizeof(int16_t));
        generate_dtmf(tone, fs, rate, '1');

        kerchunk_audio_tick_rx_out_t out;
        /* relay_active=0, drain=0 → decoder must not run, dtmf_active=0 */
        for (int i = 0; i < 5; i++) {
            kerchunk_audio_tick_rx(&s, &rx, dec, tone, fs,
                                   0 /* relay_active */, 0, 0, rate, &out);
            test_assert(out.dtmf_active == 0,
                "no detection when decoder skipped");
            test_assert(out.event == KERCHUNK_TICK_RX_NO_EVENT,
                "no event when decoder skipped");
        }

        plcode_dtmf_dec_destroy(dec);
        free(tone);
    }
    test_end();

    /* 5. COR-drop edge starts relay_drain countdown. */
    test_begin("tick_rx: COR-drop edge starts relay_drain countdown");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 1 /* relay on */);
        s.relay_drain_ms = 500;

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        int16_t *sp = calloc(fs, sizeof(int16_t));
        gen_speech_like(sp, fs);

        /* First: COR active, no drain. */
        kerchunk_audio_tick_rx_out_t out;
        kerchunk_audio_tick_rx(&s, &rx, dec, sp, fs,
                               1, 1 /* ptt_held */, KERCHUNK_AUDIO_RING_MASK,
                               rate, &out);
        test_assert(s.relay_was_active == 1, "relay_was_active tracked");
        test_assert(s.relay_drain == 0, "no drain while COR active");

        /* Now COR drops. Drain should start at rate * drain_ms / 1000. */
        kerchunk_audio_tick_rx(&s, &rx, dec, sp, fs,
                               0 /* relay_active */, 1,
                               KERCHUNK_AUDIO_RING_MASK,
                               rate, &out);
        int expected = (rate * 500) / 1000;
        /* Note: the relay path counts the frame down on the same tick,
         * so after this call relay_drain = expected - fs (if write
         * landed) OR relay_drain = expected (if write skipped). Our
         * speech-like frame is above noise floor so the early-stop
         * won't fire; the subtraction will happen. */
        test_assert(s.relay_drain == expected - fs,
            "drain started and counted down by one frame");

        plcode_dtmf_dec_destroy(dec);
        free(sp);
    }
    test_end();

    /* 6. Early stop on noise floor during drain. */
    test_begin("tick_rx: drain early-stops on low RMS");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 1);
        s.relay_drain_ms = 500;

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        int16_t *sp  = calloc(fs, sizeof(int16_t));
        int16_t *qu  = calloc(fs, sizeof(int16_t));
        gen_speech_like(sp, fs);
        gen_low_noise(qu, fs);

        /* COR active tick to establish prev state */
        kerchunk_audio_tick_rx_out_t out;
        kerchunk_audio_tick_rx(&s, &rx, dec, sp, fs,
                               1, 1, KERCHUNK_AUDIO_RING_MASK,
                               rate, &out);
        /* COR drops: drain starts */
        kerchunk_audio_tick_rx(&s, &rx, dec, qu, fs,
                               0, 1, KERCHUNK_AUDIO_RING_MASK,
                               rate, &out);
        test_assert(s.relay_drain == 0,
            "low-RMS frame triggers early stop (drain → 0)");

        plcode_dtmf_dec_destroy(dec);
        free(sp);
        free(qu);
    }
    test_end();

    /* 7. Relay disabled when software_relay=0. */
    test_begin("tick_rx: relay disabled when software_relay=0");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 0 /* relay off */);

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        int16_t *sp = calloc(fs, sizeof(int16_t));
        gen_speech_like(sp, fs);

        kerchunk_audio_tick_rx_out_t out;
        kerchunk_audio_tick_rx(&s, &rx, dec, sp, fs,
                               1 /* relay_active */, 1 /* ptt_held */,
                               KERCHUNK_AUDIO_RING_MASK, rate, &out);
        test_assert(out.relay_write == 0,
            "no relay_write when software_relay=0");
        /* Also: the COR-drop → drain-start edge should not arm drain
         * when software_relay=0. */
        kerchunk_audio_tick_rx(&s, &rx, dec, sp, fs,
                               0, 1, KERCHUNK_AUDIO_RING_MASK, rate, &out);
        test_assert(s.relay_drain == 0,
            "no drain start when software_relay=0");

        plcode_dtmf_dec_destroy(dec);
        free(sp);
    }
    test_end();

    /* 8. Relay suppressed when queue_ptt=1 (TX feedback guard). */
    test_begin("tick_rx: relay suppressed when queue_ptt=1");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 1);
        s.queue_ptt = 1;

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        int16_t *sp = calloc(fs, sizeof(int16_t));
        gen_speech_like(sp, fs);

        kerchunk_audio_tick_rx_out_t out;
        kerchunk_audio_tick_rx(&s, &rx, dec, sp, fs,
                               1, 1, KERCHUNK_AUDIO_RING_MASK, rate, &out);
        test_assert(out.relay_write == 0,
            "no relay_write during queue TX (feedback guard)");

        plcode_dtmf_dec_destroy(dec);
        free(sp);
    }
    test_end();

    /* 9. Relay write skipped if playback ring is full. */
    test_begin("tick_rx: relay skipped when play_writable < nread");
    {
        kerchunk_audio_state_t s;
        kerchunk_audio_tick_rx_state_t rx;
        init_state(&s, &rx, 1);

        plcode_dtmf_dec_t *dec = new_decoder(rate);
        int16_t *sp = calloc(fs, sizeof(int16_t));
        gen_speech_like(sp, fs);

        kerchunk_audio_tick_rx_out_t out;
        /* play_writable=0 → can't write */
        kerchunk_audio_tick_rx(&s, &rx, dec, sp, fs,
                               1, 1 /* ptt_held */, 0, rate, &out);
        test_assert(out.relay_write == 0,
            "no relay_write when ring full");

        plcode_dtmf_dec_destroy(dec);
        free(sp);
    }
    test_end();

    /* 10. NULL-safe */
    test_begin("tick_rx: NULL inputs are safe no-ops");
    {
        kerchunk_audio_tick_rx_out_t out = { .relay_write = 42 };
        kerchunk_audio_tick_rx(NULL, NULL, NULL, NULL, 0, 0, 0, 0, 48000, &out);
        test_assert(out.relay_write == 0, "NULL clears out");
        test_assert(out.dtmf_active == 0, "NULL clears dtmf_active");
        test_assert(out.event == KERCHUNK_TICK_RX_NO_EVENT, "NULL clears event");
        /* NULL out should also not crash. */
        kerchunk_audio_tick_rx(NULL, NULL, NULL, NULL, 0, 0, 0, 0, 48000, NULL);
    }
    test_end();
}
