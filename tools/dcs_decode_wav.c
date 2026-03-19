/*
 * dcs_decode_wav.c — Decode DCS from a WAV file with bit-level diagnostics.
 * Shows the raw shift register contents and what codes they match.
 *
 * Build:
 *   cc -O2 -Ilibplcode/include -Ilibplcode/src -o dcs_decode_wav \
 *      tools/dcs_decode_wav.c libplcode/libplcode.a -lm
 *
 * Run:
 *   ./dcs_decode_wav sdr_capture.wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "plcode_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void print_bits(uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; i--)
        putchar((v >> i) & 1 ? '1' : '0');
}

/* Try all possible data word extractions from a 23-bit shift register */
static void try_all_extractions(uint32_t sr)
{
    /* Standard: data at bits 22:11 */
    uint16_t d1 = (uint16_t)(sr >> 11);
    /* Reversed: data at bits 11:0 */
    uint16_t d2 = (uint16_t)(sr & 0xFFF);

    printf("    sr="); print_bits(sr, 23);
    printf("  data@hi=0x%03x(0%03o) data@lo=0x%03x(0%03o)\n",
           d1 & 0x1FF, d1 & 0x1FF, d2 & 0x1FF, d2 & 0x1FF);

    /* Check all marker positions for each extraction */
    for (int pos = 9; pos <= 11; pos++) {
        int mask_val = 1 << pos;
        int mask_check = 7 << pos; /* 3 bits at pos */
        if ((d1 & mask_check) == mask_val) {
            uint16_t code9 = d1 & 0x1FF;
            int idx = -1;
            for (int i = 0; i < PLCODE_DCS_NUM_CODES; i++)
                if (plcode_dcs_codes[i] == code9) { idx = i; break; }
            if (idx >= 0)
                printf("    HI: marker@bit%d code=0%03o label=%d *** VALID ***\n",
                       pos, code9, plcode_dcs_code_to_label(code9));
        }
    }

    /* Also try with complement (inverted DCS) */
    uint32_t comp = sr ^ 0x7FFFFF;
    uint16_t dc1 = (uint16_t)(comp >> 11);
    for (int pos = 9; pos <= 11; pos++) {
        int mask_val = 1 << pos;
        int mask_check = 7 << pos;
        if ((dc1 & mask_check) == mask_val) {
            uint16_t code9 = dc1 & 0x1FF;
            int idx = -1;
            for (int i = 0; i < PLCODE_DCS_NUM_CODES; i++)
                if (plcode_dcs_codes[i] == code9) { idx = i; break; }
            if (idx >= 0)
                printf("    HI(inv): marker@bit%d code=0%03o label=%d *** VALID INV ***\n",
                       pos, code9, plcode_dcs_code_to_label(code9));
        }
    }
}

/* Read WAV file, return samples */
static int16_t *read_wav(const char *path, int *n_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }

    uint8_t hdr[44];
    if (fread(hdr, 1, 44, fp) != 44) { fclose(fp); return NULL; }

    int rate = hdr[24] | (hdr[25]<<8) | (hdr[26]<<16) | (hdr[27]<<24);
    int bits = hdr[34] | (hdr[35]<<8);
    int data_size = hdr[40] | (hdr[41]<<8) | (hdr[42]<<16) | (hdr[43]<<24);
    int n = data_size / 2;

    printf("WAV: %d Hz, %d-bit, %d samples (%.1fs)\n", rate, bits, n, (double)n/rate);

    int16_t *buf = malloc(n * sizeof(int16_t));
    if (fread(buf, sizeof(int16_t), n, fp) != (size_t)n)
        printf("Warning: short read\n");
    fclose(fp);
    *n_out = n;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <wav_file>\n", argv[0]);
        return 1;
    }

    plcode_tables_init();

    /* Print what code 025 should look like */
    int idx025 = plcode_dcs_code_index(25);
    uint32_t cw025 = plcode_dcs_codewords[idx025];
    printf("Expected DCS 025 codeword: 0x%06x = ", cw025);
    print_bits(cw025, 23);
    printf("\n\n");

    int n;
    int16_t *samples = read_wav(argv[1], &n);
    if (!samples) return 1;

    /* Manual DCS decoder with verbose output */
    int rate = 8000;

    /* Butterworth LPF at 300 Hz */
    double wc = tan(M_PI * 300.0 / rate);
    double wc2 = wc * wc;
    double sq2 = 1.4142135623730951;
    double norm = 1.0 / (1.0 + sq2 * wc + wc2);
    double b0 = wc2 * norm, b1 = 2*b0, b2 = b0;
    double a1 = 2.0*(wc2-1.0)*norm, a2 = (1.0-sq2*wc+wc2)*norm;

    double x1=0, x2=0, y1=0, y2=0;

    /* PLL */
    double pll_phase = 0;
    double pll_inc = 134.4 / rate;
    int prev_bit = 0;

    /* Shift registers — try both shift directions */
    uint32_t sr_left = 0;   /* left-shift (MSB first receive) */
    uint32_t sr_right = 0;  /* right-shift (LSB first receive) */
    int total_bits = 0;

    int match_count = 0;
    int last_match_code = -1;

    printf("\nProcessing %d samples...\n", n);
    printf("Showing first 50 bit samples:\n\n");

    for (int i = 0; i < n; i++) {
        /* LPF */
        double x = (double)samples[i];
        double y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;

        /* Comparator */
        int bit;
        if (y > 500) bit = 1;
        else if (y < -500) bit = 0;
        else bit = prev_bit;

        /* PLL edge sync */
        if (bit != prev_bit) {
            double phase_err = pll_phase - 0.5;
            pll_phase -= phase_err * 0.06;
        }
        prev_bit = bit;

        /* PLL clock */
        double prev_phase = pll_phase;
        pll_phase += pll_inc;

        if (prev_phase < 0.5 && pll_phase >= 0.5) {
            /* Sample bit */
            sr_left = ((sr_left << 1) | (uint32_t)bit) & 0x7FFFFF;
            sr_right = ((sr_right >> 1) | ((uint32_t)bit << 22)) & 0x7FFFFF;
            total_bits++;

            if (total_bits <= 200) {
                printf("  bit[%3d] = %d  t=%.3fs  sr_R=",
                       total_bits, bit, (double)i/rate);
                print_bits(sr_right, 23);
                printf("\n");
            }

            if (total_bits >= 23 && total_bits % 23 == 0) {
                printf("\n  === Checking at bit %d (codeword boundary) ===\n", total_bits);
                printf("  Left-shift SR:\n");
                try_all_extractions(sr_left);
                printf("  Right-shift SR:\n");
                try_all_extractions(sr_right);

                /* Also try with inverted audio polarity */
                printf("  Left-shift SR (inverted polarity):\n");
                uint32_t sr_left_inv = 0;
                /* Can't easily reconstruct, but complement approximates */
                try_all_extractions(sr_left ^ 0x7FFFFF);
            }
        }

        if (pll_phase >= 1.0) pll_phase -= 1.0;
    }

    printf("\nTotal bits decoded: %d (%.1f codewords)\n",
           total_bits, total_bits / 23.0);

    /* Now run the actual libplcode decoder for comparison */
    printf("\n=== libplcode DCS decoder result ===\n");
    plcode_dcs_dec_t *dec = NULL;
    plcode_dcs_dec_create(&dec, rate);
    plcode_dcs_result_t res;
    memset(&res, 0, sizeof(res));
    plcode_dcs_dec_process(dec, samples, (size_t)n, &res);
    printf("detected=%d code=%03d inv=%d\n", res.detected, res.code_number, res.inverted);

    /* Also try inverted audio */
    printf("\n=== libplcode DCS decoder (inverted audio) ===\n");
    int16_t *inv = malloc(n * sizeof(int16_t));
    for (int i = 0; i < n; i++) inv[i] = -samples[i];
    plcode_dcs_dec_t *dec_inv = NULL;
    plcode_dcs_dec_create(&dec_inv, rate);
    plcode_dcs_result_t res_inv;
    memset(&res_inv, 0, sizeof(res_inv));
    plcode_dcs_dec_process(dec_inv, inv, (size_t)n, &res_inv);
    printf("detected=%d code=%03d inv=%d\n", res_inv.detected, res_inv.code_number, res_inv.inverted);

    plcode_dcs_dec_destroy(dec);
    plcode_dcs_dec_destroy(dec_inv);
    free(inv);
    free(samples);
    return 0;
}
