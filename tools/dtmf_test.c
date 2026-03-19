/*
 * dtmf_test.c — Generate DTMF tones and test the Goertzel detector.
 * Validates the detector logic offline without an SDR.
 *
 * Build:
 *   cc -O2 -o dtmf_test tools/dtmf_test.c -lm
 *
 * Run:
 *   ./dtmf_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#define AUDIO_RATE 8000
#define DTMF_BLOCK (AUDIO_RATE / 50)  /* 160 samples = 20ms */
#define AMPLITUDE  8000

/* ── Goertzel ── */

typedef struct {
    double coeff;
    double s1, s2;
    int    count;
    int    block_size;
} goertzel_t;

static void goertzel_init(goertzel_t *g, double freq_hz, int rate, int block)
{
    double k = 0.5 + (block * freq_hz / rate);
    g->coeff = 2.0 * cos(2.0 * M_PI * k / block);
    g->s1 = g->s2 = 0;
    g->count = 0;
    g->block_size = block;
}

static void goertzel_reset(goertzel_t *g)
{
    g->s1 = g->s2 = 0;
    g->count = 0;
}

/* Returns power when block completes, -1 otherwise */
static double goertzel_process(goertzel_t *g, const int16_t *buf, int n)
{
    for (int i = 0; i < n; i++) {
        double s0 = (double)buf[i] + g->coeff * g->s1 - g->s2;
        g->s2 = g->s1;
        g->s1 = s0;
        g->count++;
        if (g->count >= g->block_size) {
            double power = g->s1 * g->s1 + g->s2 * g->s2
                         - g->coeff * g->s1 * g->s2;
            g->s1 = g->s2 = 0;
            g->count = 0;
            return power;
        }
    }
    return -1;
}

/* ── DTMF frequencies ── */

static const double dtmf_row[] = { 697.0, 770.0, 852.0, 941.0 };
static const double dtmf_col[] = { 1209.0, 1336.0, 1477.0, 1633.0 };
static const char   dtmf_map[4][4] = {
    {'1','2','3','A'},
    {'4','5','6','B'},
    {'7','8','9','C'},
    {'*','0','#','D'}
};

/* Find digit from row/col indices */
static void dtmf_indices(char digit, int *row_out, int *col_out)
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            if (dtmf_map[r][c] == digit) {
                *row_out = r; *col_out = c; return;
            }
    *row_out = -1; *col_out = -1;
}

/* Generate DTMF tone for a digit */
static void gen_dtmf(int16_t *buf, int n, char digit)
{
    int ri, ci;
    dtmf_indices(digit, &ri, &ci);
    if (ri < 0) { memset(buf, 0, n * sizeof(int16_t)); return; }

    double f1 = dtmf_row[ri];
    double f2 = dtmf_col[ci];

    for (int i = 0; i < n; i++) {
        double t = (double)i / AUDIO_RATE;
        double s = sin(2.0 * M_PI * f1 * t) + sin(2.0 * M_PI * f2 * t);
        buf[i] = (int16_t)(s * AMPLITUDE / 2);
    }
}

/* Detect DTMF from a buffer using Goertzel */
static char detect_dtmf(const int16_t *buf, int n)
{
    goertzel_t row_det[4], col_det[4];
    for (int i = 0; i < 4; i++) {
        goertzel_init(&row_det[i], dtmf_row[i], AUDIO_RATE, n);
        goertzel_init(&col_det[i], dtmf_col[i], AUDIO_RATE, n);
    }

    double row_power[4], col_power[4];
    for (int i = 0; i < 4; i++) {
        row_power[i] = goertzel_process(&row_det[i], buf, n);
        col_power[i] = goertzel_process(&col_det[i], buf, n);
    }

    /* Find strongest row and column */
    int best_r = 0, best_c = 0;
    for (int r = 1; r < 4; r++)
        if (row_power[r] > row_power[best_r]) best_r = r;
    for (int c = 1; c < 4; c++)
        if (col_power[c] > col_power[best_c]) best_c = c;

    printf("    Row powers: ");
    for (int r = 0; r < 4; r++)
        printf("%.0fHz=%.0e%s ", dtmf_row[r], row_power[r], r == best_r ? "*" : "");
    printf("\n    Col powers: ");
    for (int c = 0; c < 4; c++)
        printf("%.0fHz=%.0e%s ", dtmf_col[c], col_power[c], c == best_c ? "*" : "");
    printf("\n");

    double rp = row_power[best_r];
    double cp = col_power[best_c];

    /* Check both above noise floor */
    double noise = 0;
    for (int r = 0; r < 4; r++)
        if (r != best_r && row_power[r] > noise) noise = row_power[r];
    for (int c = 0; c < 4; c++)
        if (c != best_c && col_power[c] > noise) noise = col_power[c];

    double row_snr = (noise > 0) ? 10.0 * log10(rp / noise) : 99.0;
    double col_snr = (noise > 0) ? 10.0 * log10(cp / noise) : 99.0;
    double twist = 10.0 * log10(rp > cp ? rp / cp : cp / rp);

    printf("    SNR: row=%.1f dB  col=%.1f dB  twist=%.1f dB\n",
           row_snr, col_snr, twist);

    if (row_snr < 6.0 || col_snr < 6.0) return '?';
    if (twist > 6.0) return '?';

    return dtmf_map[best_r][best_c];
}

int main(void)
{
    printf("DTMF Goertzel Detector Test\n");
    printf("===========================\n");
    printf("Audio rate: %d Hz, block size: %d samples (%.0f ms)\n\n",
           AUDIO_RATE, DTMF_BLOCK, 1000.0 * DTMF_BLOCK / AUDIO_RATE);

    /* Test all 16 DTMF digits */
    const char *digits = "123A456B789C*0#D";
    int pass = 0, fail = 0;

    for (int d = 0; d < 16; d++) {
        char expected = digits[d];
        int16_t buf[DTMF_BLOCK];
        gen_dtmf(buf, DTMF_BLOCK, expected);

        printf("Digit '%c': generating %.0f + %.0f Hz\n",
               expected, dtmf_row[d / 4], dtmf_col[d % 4]);

        char detected = detect_dtmf(buf, DTMF_BLOCK);

        if (detected == expected) {
            printf("    PASS: detected '%c'\n\n", detected);
            pass++;
        } else {
            printf("    FAIL: expected '%c', got '%c'\n\n", expected, detected);
            fail++;
        }
    }

    /* Test with silence (should not detect) */
    printf("Silence:\n");
    {
        int16_t buf[DTMF_BLOCK];
        memset(buf, 0, sizeof(buf));
        char detected = detect_dtmf(buf, DTMF_BLOCK);
        printf("    %s: got '%c'\n\n",
               detected == '?' ? "PASS" : "FAIL", detected);
        if (detected == '?') pass++; else fail++;
    }

    /* Test with a single tone (should not detect — no pair) */
    printf("Single tone (697 Hz only):\n");
    {
        int16_t buf[DTMF_BLOCK];
        for (int i = 0; i < DTMF_BLOCK; i++)
            buf[i] = (int16_t)(AMPLITUDE * sin(2.0 * M_PI * 697.0 * i / AUDIO_RATE));
        char detected = detect_dtmf(buf, DTMF_BLOCK);
        printf("    %s: got '%c'\n\n",
               detected == '?' ? "PASS" : "FAIL", detected);
        if (detected == '?') pass++; else fail++;
    }

    /* Test with longer block (40ms) for comparison */
    printf("\n--- Longer block test (40ms = %d samples) ---\n\n", DTMF_BLOCK * 2);
    for (int d = 0; d < 4; d++) {
        char expected = digits[d];
        int n = DTMF_BLOCK * 2;
        int16_t *buf = malloc(n * sizeof(int16_t));
        gen_dtmf(buf, n, expected);
        printf("Digit '%c' (40ms):\n", expected);
        char detected = detect_dtmf(buf, n);
        printf("    %s: detected '%c'\n\n",
               detected == expected ? "PASS" : "FAIL", detected);
        if (detected == expected) pass++; else fail++;
        free(buf);
    }

    printf("===========================\n");
    printf("Results: %d/%d passed\n", pass, pass + fail);

    return fail > 0 ? 1 : 0;
}
