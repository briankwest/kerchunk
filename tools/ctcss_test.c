/*
 * ctcss_test.c — CTCSS detector diagnostic.
 * Shows Goertzel power for all 39 tones so we can see the SNR.
 *
 * Build:
 *   cc -O2 -o ctcss_test tools/ctcss_test.c -lrtlsdr -lm
 *
 * Run:
 *   ./ctcss_test [freq_hz]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <rtl-sdr.h>

#define SAMPLE_RATE  240000
#define AUDIO_RATE   8000
#define DEC_FACTOR   (SAMPLE_RATE / AUDIO_RATE)

static volatile int g_running = 1;
static void sighandler(int sig) { (void)sig; g_running = 0; }

/* Goertzel */
typedef struct {
    double coeff;
    double s1, s2;
    int    count;
    int    block_size;
    double last_power;
} goertzel_t;

static void goertzel_init(goertzel_t *g, double freq_hz, int rate, int block)
{
    double k = 0.5 + (block * freq_hz / rate);
    g->coeff = 2.0 * cos(2.0 * M_PI * k / block);
    g->s1 = g->s2 = 0;
    g->count = 0;
    g->block_size = block;
    g->last_power = 0;
}

static int goertzel_feed(goertzel_t *g, const int16_t *buf, int n)
{
    int completed = 0;
    for (int i = 0; i < n; i++) {
        double s0 = (double)buf[i] + g->coeff * g->s1 - g->s2;
        g->s2 = g->s1;
        g->s1 = s0;
        g->count++;
        if (g->count >= g->block_size) {
            g->last_power = g->s1 * g->s1 + g->s2 * g->s2
                          - g->coeff * g->s1 * g->s2;
            g->s1 = g->s2 = 0;
            g->count = 0;
            completed = 1;
        }
    }
    return completed;
}

/* CTCSS tones */
static const double ctcss_tones[] = {
     67.0,  71.9,  74.4,  77.0,  79.7,  82.5,  85.4,  88.5,  91.5,  94.8,
     97.4, 100.0, 103.5, 107.2, 110.9, 114.8, 118.8, 123.0, 127.3, 131.8,
    136.5, 141.3, 146.2, 151.4, 156.7, 162.2, 167.9, 173.8, 179.9, 186.2,
    192.8, 203.5, 210.7, 218.1, 225.7, 233.6, 241.8, 250.3, 254.1
};
#define NUM_CTCSS (sizeof(ctcss_tones) / sizeof(ctcss_tones[0]))

int main(int argc, char **argv)
{
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    uint32_t freq = 462562500;
    if (argc >= 2) freq = (uint32_t)atof(argv[1]);

    int count = rtlsdr_get_device_count();
    if (count == 0) { fprintf(stderr, "No RTL-SDR\n"); return 1; }

    rtlsdr_dev_t *dev;
    if (rtlsdr_open(&dev, 0) < 0) { fprintf(stderr, "Open failed\n"); return 1; }

    rtlsdr_set_center_freq(dev, freq);
    rtlsdr_set_sample_rate(dev, SAMPLE_RATE);
    rtlsdr_set_tuner_gain_mode(dev, 0);
    rtlsdr_reset_buffer(dev);

    printf("CTCSS Diagnostic — %.4f MHz\n", freq / 1e6);
    printf("Showing Goertzel power for all %d tones (1s blocks)\n", (int)NUM_CTCSS);
    printf("A real CTCSS tone should be 10+ dB above all others.\n\n");

    /* Use 1-second blocks for good frequency resolution */
    int ctcss_block = AUDIO_RATE;  /* 8000 samples = 1 second */
    goertzel_t det[NUM_CTCSS];
    for (int i = 0; i < (int)NUM_CTCSS; i++)
        goertzel_init(&det[i], ctcss_tones[i], AUDIO_RATE, ctcss_block);

    int iq_bytes = 16384;
    unsigned char *iq_buf = malloc(iq_bytes);
    int max_audio = iq_bytes / 2 / DEC_FACTOR + 1;
    int16_t *audio_buf = malloc(max_audio * sizeof(int16_t));

    float prev_i = 0, prev_q = 0, deemph = 0;
    float dec_acc = 0;
    int dec_count = 0;

    while (g_running) {
        int n_read = 0;
        if (rtlsdr_read_sync(dev, iq_buf, iq_bytes, &n_read) < 0) break;
        if (n_read == 0) continue;

        int nsamples = n_read / 2;
        int audio_pos = 0;

        for (int i = 0; i < nsamples; i++) {
            float si = ((float)iq_buf[i*2] - 127.5f) / 127.5f;
            float sq = ((float)iq_buf[i*2+1] - 127.5f) / 127.5f;
            float dot = si * prev_i + sq * prev_q;
            float cross = sq * prev_i - si * prev_q;
            float fm = atan2f(cross, dot);
            prev_i = si; prev_q = sq;
            float audio = fm * (16000.0f / 0.07f);
            float alpha = 1.0f / (1.0f + (float)SAMPLE_RATE * 75e-6f * 2.0f * (float)M_PI);
            deemph = deemph + alpha * (audio - deemph);
            dec_acc += deemph;
            dec_count++;
            if (dec_count >= DEC_FACTOR) {
                audio_buf[audio_pos++] = (int16_t)(dec_acc / DEC_FACTOR);
                dec_acc = 0; dec_count = 0;
            }
        }

        if (audio_pos == 0) continue;

        /* RMS */
        int64_t sum = 0;
        for (int i = 0; i < audio_pos; i++) {
            int32_t v = audio_buf[i]; sum += v * v;
        }
        int32_t rms = 0;
        { int64_t a = sum / audio_pos; while ((int64_t)rms*rms < a) rms++; }

        /* Feed all detectors */
        int completed = 0;
        for (int t = 0; t < (int)NUM_CTCSS; t++)
            completed |= goertzel_feed(&det[t], audio_buf, audio_pos);

        if (!completed) continue;

        /* Find best and compute SNR */
        int best = 0;
        double best_power = det[0].last_power;
        for (int t = 1; t < (int)NUM_CTCSS; t++) {
            if (det[t].last_power > best_power) {
                best_power = det[t].last_power;
                best = t;
            }
        }

        /* Second best */
        double second = 0;
        for (int t = 0; t < (int)NUM_CTCSS; t++) {
            if (t != best && det[t].last_power > second)
                second = det[t].last_power;
        }

        double snr = (second > 0) ? 10.0 * log10(best_power / second) : 99.0;

        time_t now = time(NULL);
        struct tm *tm = localtime(&now);

        printf("%02d:%02d:%02d  RMS=%-5d  Best=%.1f Hz  power=%.2e  2nd=%.2e  SNR=%.1f dB  %s\n",
               tm->tm_hour, tm->tm_min, tm->tm_sec,
               rms,
               ctcss_tones[best], best_power, second, snr,
               snr >= 10.0 ? "<== DETECTED" : "(noise)");

        /* Show top 5 tones for debugging */
        typedef struct { int idx; double power; } tp_t;
        tp_t top5[5] = {{0,0},{0,0},{0,0},{0,0},{0,0}};
        for (int t = 0; t < (int)NUM_CTCSS; t++) {
            double p = det[t].last_power;
            for (int k = 0; k < 5; k++) {
                if (p > top5[k].power) {
                    for (int j = 4; j > k; j--) top5[j] = top5[j-1];
                    top5[k].idx = t;
                    top5[k].power = p;
                    break;
                }
            }
        }
        printf("         Top 5: ");
        for (int k = 0; k < 5; k++)
            printf("%.1fHz=%.1e  ", ctcss_tones[top5[k].idx], top5[k].power);
        printf("\n\n");
    }

    printf("Shutting down...\n");
    rtlsdr_close(dev);
    free(iq_buf);
    free(audio_buf);
    return 0;
}
