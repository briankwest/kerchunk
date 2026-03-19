/*
 * dcs_bruteforce.c — Test DCS decode against a WAV capture using libplcode.
 * Runs both normal and inverted audio through the decoder with orbit
 * canonicalization.
 *
 * Build:
 *   cc -O2 -Iinclude -Ilibplcode/include -o dcs_bruteforce \
 *      tools/dcs_bruteforce.c libplcode/libplcode.a -lm
 *
 * Run:
 *   ./dcs_bruteforce sdr_capture.wav
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <plcode.h>

static int16_t *read_wav(const char *path, int *n_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", path); return NULL; }
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, fp) != 44) { fclose(fp); return NULL; }
    int rate = hdr[24] | (hdr[25]<<8) | (hdr[26]<<16) | (hdr[27]<<24);
    int data_size = hdr[40] | (hdr[41]<<8) | (hdr[42]<<16) | (hdr[43]<<24);
    int n = data_size / 2;
    printf("WAV: %d Hz, %d samples (%.1fs)\n\n", rate, n, (double)n / rate);
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

    int n;
    int16_t *samples = read_wav(argv[1], &n);
    if (!samples) return 1;

    /* Normal polarity */
    printf("=== Normal FM polarity ===\n");
    {
        plcode_dcs_dec_t *dec = NULL;
        plcode_dcs_dec_create(&dec, 8000);
        plcode_dcs_result_t res;
        memset(&res, 0, sizeof(res));
        plcode_dcs_dec_process(dec, samples, (size_t)n, &res);
        if (res.detected)
            printf("  Detected: DCS %03d %s\n", res.code_number,
                   res.inverted ? "(inverted)" : "(normal)");
        else
            printf("  Not detected\n");
        plcode_dcs_dec_destroy(dec);
    }

    /* Inverted polarity */
    printf("\n=== Inverted FM polarity ===\n");
    {
        int16_t *inv = malloc(n * sizeof(int16_t));
        for (int i = 0; i < n; i++) inv[i] = -samples[i];

        plcode_dcs_dec_t *dec = NULL;
        plcode_dcs_dec_create(&dec, 8000);
        plcode_dcs_result_t res;
        memset(&res, 0, sizeof(res));
        plcode_dcs_dec_process(dec, inv, (size_t)n, &res);
        if (res.detected)
            printf("  Detected: DCS %03d %s\n", res.code_number,
                   res.inverted ? "(inverted)" : "(normal)");
        else
            printf("  Not detected\n");
        plcode_dcs_dec_destroy(dec);
        free(inv);
    }

    free(samples);
    printf("\nDone.\n");
    return 0;
}
