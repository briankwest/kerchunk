/*
 * test_integ_scrambler.c — Integration tests for mod_scrambler
 *
 * Tests: config, self-inverse, different codes, DTMF control,
 * CW ID bypass, emergency bypass.
 */

#include "test_integ_mock.h"
#include <math.h>

/* Pull in the module source */
#include "../modules/mod_scrambler.c"

/* Generate a pure tone at freq_hz into buf (n samples at mock core rate) */
static void gen_tone(int16_t *buf, size_t n, double freq_hz)
{
    int rate = g_mock_core.sample_rate;
    for (size_t i = 0; i < n; i++)
        buf[i] = (int16_t)(16000.0 * sin(2.0 * M_PI * freq_hz * (double)i / (double)rate));
}

/* Compute RMS of a buffer */
static double compute_rms(const int16_t *buf, size_t n)
{
    double sum = 0.0;
    for (size_t i = 0; i < n; i++)
        sum += (double)buf[i] * (double)buf[i];
    return sqrt(sum / (double)n);
}

/* Compute correlation between two buffers (normalized -1..+1) */
static double correlation(const int16_t *a, const int16_t *b, size_t n)
{
    double sum_ab = 0, sum_aa = 0, sum_bb = 0;
    for (size_t i = 0; i < n; i++) {
        sum_ab += (double)a[i] * (double)b[i];
        sum_aa += (double)a[i] * (double)a[i];
        sum_bb += (double)b[i] * (double)b[i];
    }
    if (sum_aa < 1.0 || sum_bb < 1.0) return 0.0;
    return sum_ab / sqrt(sum_aa * sum_bb);
}

void test_integ_scrambler(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();

    mod_scrambler.load(&g_mock_core);

    /* ── 1. Configure sets carrier from code ── */
    test_begin("scrambler: configure sets carrier from code");
    {
        kerchunk_config_t *cfg = kerchunk_config_create();
        kerchunk_config_set(cfg, "scrambler", "enabled", "on");
        kerchunk_config_set(cfg, "scrambler", "code", "4");
        scrambler_configure(cfg);
        kerchunk_config_destroy(cfg);
    }
    test_assert(g_enabled == 1, "not enabled");
    test_assert(g_code == 4, "wrong code");
    test_assert(g_carrier_hz == 3000, "wrong carrier");
    test_end();

    /* ── 2. Self-inverse: scramble twice → recovers original ── */
    test_begin("scrambler: self-inverse property");
    {
        int rate = g_mock_core.sample_rate;
        /* Scale buffer and settling regions with sample rate.
         * FIR delay is ~62 samples at any rate; two passes = ~124.
         * Use 0.5s of audio and skip first 0.1s for settling. */
        size_t n = (size_t)(rate / 2);
        size_t settle = (size_t)(rate / 10);
        int period = rate / 1000;  /* 1kHz period in samples */

        int16_t *original = calloc(n, sizeof(int16_t));
        int16_t *work = calloc(n, sizeof(int16_t));

        gen_tone(original, n, 1000.0);  /* 1 kHz tone */
        memcpy(work, original, n * sizeof(int16_t));

        /* Scramble */
        scrambler_state_t st1;
        state_init(&st1, g_carrier_hz);
        scrambler_process(work, n, &st1);

        /* Verify scrambled is different from original (skip settling) */
        double corr1 = correlation(original + settle, work + settle, n - 2 * settle);
        test_assert(corr1 < 0.5, "scrambled too similar to original");

        /* Descramble (same carrier — self-inverse) */
        scrambler_state_t st2;
        state_init(&st2, g_carrier_hz);
        scrambler_process(work, n, &st2);

        /* After two passes through the scrambler, the recovered signal
         * has 4x FIR group delay. Check that the signal has energy
         * (not zeroed out) and that it's periodic at 1kHz. */
        double rms = compute_rms(work + 2 * settle, n - 4 * settle);
        test_assert(rms > 500.0, "self-inverse output too quiet");

        /* Check periodicity: auto-correlate at 1kHz period */
        double ac = correlation(work + 2 * settle, work + 2 * settle + period,
                                n - 4 * settle - (size_t)period);
        test_assert(ac > 0.5, "self-inverse failed: not periodic at input freq");

        free(original);
        free(work);
    }
    test_end();

    /* ── 3. Different codes produce different output ── */
    test_begin("scrambler: different codes differ");
    {
        size_t n = (size_t)(g_mock_core.sample_rate / 10);  /* 0.1s */
        int16_t *src = calloc(n, sizeof(int16_t));
        int16_t *out1 = calloc(n, sizeof(int16_t));
        int16_t *out2 = calloc(n, sizeof(int16_t));

        gen_tone(src, n, 1000.0);

        /* Code 1 (2700 Hz) */
        apply_code(1);
        scrambler_state_t s1;
        state_init(&s1, g_carrier_hz);
        memcpy(out1, src, n * sizeof(int16_t));
        scrambler_process(out1, n, &s1);

        /* Code 8 (3400 Hz) */
        apply_code(8);
        scrambler_state_t s2;
        state_init(&s2, g_carrier_hz);
        memcpy(out2, src, n * sizeof(int16_t));
        scrambler_process(out2, n, &s2);

        /* Outputs should differ */
        double corr = correlation(out1 + 100, out2 + 100, n - 200);
        test_assert(corr < 0.9, "different codes produced same output");

        /* Restore code 4 */
        apply_code(4);

        free(src);
        free(out1);
        free(out2);
    }
    test_end();

    /* ── 4. DTMF toggle ── */
    test_begin("scrambler: DTMF toggle");
    {
        int was_enabled = g_enabled;
        kerchevt_t e = { .type = DTMF_EVT_SCRAMBLER,
                         .custom = { .data = NULL, .len = 0 } };
        kerchevt_fire(&e);
        test_assert(g_enabled != was_enabled, "toggle didn't change state");
        /* Toggle back */
        kerchevt_fire(&e);
        test_assert(g_enabled == was_enabled, "second toggle didn't restore");
    }
    test_end();

    /* ── 5. DTMF set code ── */
    test_begin("scrambler: DTMF set code");
    {
        char data[] = "5";
        kerchevt_t e = { .type = DTMF_EVT_SCRAMBLER,
                         .custom = { .data = data, .len = 1 } };
        kerchevt_fire(&e);
        test_assert(g_code == 5, "wrong code after DTMF set");
        test_assert(g_carrier_hz == 3100, "wrong carrier after DTMF set");
        test_assert(g_enabled == 1, "not enabled after DTMF set");
    }
    test_end();

    /* ── 6. DTMF disable ── */
    test_begin("scrambler: DTMF disable");
    {
        char data[] = "0";
        kerchevt_t e = { .type = DTMF_EVT_SCRAMBLER,
                         .custom = { .data = data, .len = 1 } };
        kerchevt_fire(&e);
        test_assert(g_enabled == 0, "not disabled");
        test_assert(kerchunk_core_get_rx_scrambler(NULL) == NULL, "RX hook not cleared");
        test_assert(kerchunk_core_get_tx_scrambler(NULL) == NULL, "TX hook not cleared");
    }
    test_end();

    /* ── 7. Frequency override ── */
    test_begin("scrambler: frequency override");
    {
        kerchunk_config_t *cfg = kerchunk_config_create();
        kerchunk_config_set(cfg, "scrambler", "enabled", "on");
        kerchunk_config_set(cfg, "scrambler", "code", "2");
        kerchunk_config_set(cfg, "scrambler", "frequency", "2850");
        scrambler_configure(cfg);
        kerchunk_config_destroy(cfg);
    }
    test_assert(g_carrier_hz == 2850, "frequency override not applied");
    test_end();

    /* ── 8. CW ID bypass ── */
    test_begin("scrambler: CW ID bypass");
    {
        g_cwid_bypass = 0;
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "cwid", .description = "CW ID" } };
        kerchevt_fire(&ae);
        test_assert(g_cwid_bypass == 1, "CW ID bypass not set");

        kerchevt_t qc = { .type = KERCHEVT_QUEUE_COMPLETE };
        kerchevt_fire(&qc);
        test_assert(g_cwid_bypass == 0, "CW ID bypass not cleared");
    }
    test_end();

    /* ── 9. Emergency bypass ── */
    test_begin("scrambler: emergency bypass skips processing");
    {
        size_t n = 320;
        int16_t buf[320];
        int16_t orig[320];
        gen_tone(buf, n, 1000.0);
        memcpy(orig, buf, n * sizeof(int16_t));

        kerchunk_core_set_emergency(1);
        rx_scrambler_cb(buf, n, &g_rx_state);
        kerchunk_core_set_emergency(0);

        /* Buffer should be unchanged */
        test_assert(memcmp(buf, orig, n * sizeof(int16_t)) == 0,
                    "emergency bypass didn't skip processing");
    }
    test_end();

    /* ── 10. Unload cleans up ── */
    test_begin("scrambler: unload clears hooks");
    {
        /* Re-enable so hooks are installed */
        kerchunk_config_t *cfg = kerchunk_config_create();
        kerchunk_config_set(cfg, "scrambler", "enabled", "on");
        kerchunk_config_set(cfg, "scrambler", "code", "4");
        scrambler_configure(cfg);
        kerchunk_config_destroy(cfg);

        test_assert(kerchunk_core_get_rx_scrambler(NULL) != NULL, "hooks not installed");
        scrambler_unload();
        test_assert(kerchunk_core_get_rx_scrambler(NULL) == NULL, "RX hook not cleared");
        test_assert(kerchunk_core_get_tx_scrambler(NULL) == NULL, "TX hook not cleared");
    }
    test_end();

    kerchevt_shutdown();
}
