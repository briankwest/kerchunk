/*
 * sdr_test.c — Standalone RTL-SDR test: tune to a frequency,
 * FM demodulate, detect CTCSS/DCS/DTMF using libplcode.
 *
 * Build:
 *   cc -O2 -Iinclude -Ilibplcode/include -o sdr_test tools/sdr_test.c \
 *      libplcode/libplcode.a -lrtlsdr -lm
 *
 * Run:
 *   ./sdr_test [freq_hz]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <plcode.h>
#include <rtl-sdr.h>

#define DEFAULT_FREQ 462562500   /* 462.5625 MHz — GMRS/FRS channel 1 */
#define SAMPLE_RATE  240000
#define AUDIO_RATE   8000
#define DEC_FACTOR   (SAMPLE_RATE / AUDIO_RATE)  /* 30 */

/* FM squelch: signal = LOW RMS (quieting), noise = HIGH RMS */
static int SQUELCH_OPEN  = 15000;
static int SQUELCH_CLOSE = 17000;

static volatile int g_running = 1;
static void sighandler(int sig) { (void)sig; g_running = 0; }

int main(int argc, char **argv)
{
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    uint32_t freq = DEFAULT_FREQ;
    if (argc >= 2) freq = (uint32_t)atof(argv[1]);
    if (argc >= 3) SQUELCH_OPEN = atoi(argv[2]);
    if (argc >= 4) SQUELCH_CLOSE = atoi(argv[3]);
    if (SQUELCH_CLOSE <= SQUELCH_OPEN) SQUELCH_CLOSE = SQUELCH_OPEN + 2000;

    int count = rtlsdr_get_device_count();
    if (count == 0) { fprintf(stderr, "No RTL-SDR devices found.\n"); return 1; }
    printf("Found %d device(s): %s\n", count, rtlsdr_get_device_name(0));

    rtlsdr_dev_t *dev;
    if (rtlsdr_open(&dev, 0) < 0) { fprintf(stderr, "Open failed\n"); return 1; }

    rtlsdr_set_center_freq(dev, freq);
    rtlsdr_set_sample_rate(dev, SAMPLE_RATE);
    rtlsdr_set_tuner_gain_mode(dev, 0);
    rtlsdr_reset_buffer(dev);

    printf("Tuned: %.4f MHz  Rate: %d  Audio: %d Hz\n", freq / 1e6, SAMPLE_RATE, AUDIO_RATE);
    printf("Squelch: open < %d  close > %d  (FM-inverted)\n", SQUELCH_OPEN, SQUELCH_CLOSE);
    printf("Usage: %s [freq_hz] [squelch_open] [squelch_close]\n", argv[0]);
    printf("Ctrl-C to stop.\n\n");

    /* Create libplcode decoders */
    plcode_ctcss_dec_t *ctcss_dec = NULL;
    plcode_dcs_dec_t   *dcs_dec   = NULL;
    plcode_dcs_dec_t   *dcs_dec_inv = NULL;  /* inverted polarity */
    plcode_dtmf_dec_t  *dtmf_dec  = NULL;
    plcode_ctcss_dec_create(&ctcss_dec, AUDIO_RATE);
    plcode_dcs_dec_create(&dcs_dec, AUDIO_RATE);
    plcode_dcs_dec_create(&dcs_dec_inv, AUDIO_RATE);
    plcode_dtmf_dec_create(&dtmf_dec, AUDIO_RATE);

    /* WAV recording of demodulated audio */
    FILE *wav_fp = fopen("sdr_capture.wav", "wb");
    uint32_t wav_samples = 0;
    if (wav_fp) {
        /* Write placeholder header — update on close */
        uint8_t hdr[44] = {
            'R','I','F','F', 0,0,0,0, 'W','A','V','E',
            'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
            0x40,0x1F,0,0, /* 8000 Hz */
            0x80,0x3E,0,0, /* 8000*2 bytes/sec */
            2,0, 16,0,
            'd','a','t','a', 0,0,0,0
        };
        fwrite(hdr, 1, 44, wav_fp);
        printf("Recording demodulated audio to sdr_capture.wav\n");
    }

    /* Buffers */
    int iq_bytes = 16384;
    unsigned char *iq_buf = malloc(iq_bytes);
    int max_audio = iq_bytes / 2 / DEC_FACTOR + 1;
    int16_t *audio_buf = malloc(max_audio * sizeof(int16_t));

    /* FM demod state */
    float prev_i = 0, prev_q = 0, deemph = 0;
    float dec_acc = 0;
    int   dec_count = 0;

    /* Squelch state with debounce */
    int    squelch_open = 0;
    time_t open_time = 0;
    int    sq_open_count = 0;    /* consecutive frames below open threshold */
    int    sq_close_count = 0;   /* consecutive frames above close threshold */
#define SQ_DEBOUNCE 5  /* require N consecutive frames to transition */

    /* Detection state */
    double detected_ctcss = 0;
    int    detected_dcs = 0;
    int    detected_dcs_inv = 0;
    char   dtmf_buf[64];
    int    dtmf_pos = 0;
    int    prev_ctcss = 0;
    int    prev_dcs = 0;
    int    prev_dtmf = 0;

    int    frame_count = 0;

    while (g_running) {
        int n_read = 0;
        if (rtlsdr_read_sync(dev, iq_buf, iq_bytes, &n_read) < 0) break;
        if (n_read == 0) continue;

        int nsamples = n_read / 2;
        int audio_pos = 0;

        /* IQ → FM demod → decimate to 8 kHz */
        for (int i = 0; i < nsamples; i++) {
            float si = ((float)iq_buf[i * 2]     - 127.5f) / 127.5f;
            float sq = ((float)iq_buf[i * 2 + 1] - 127.5f) / 127.5f;

            float dot   = si * prev_i + sq * prev_q;
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
                dec_acc = 0;
                dec_count = 0;
            }
        }
        if (audio_pos == 0) continue;

        /* Write demodulated audio to WAV */
        if (wav_fp) {
            fwrite(audio_buf, sizeof(int16_t), (size_t)audio_pos, wav_fp);
            wav_samples += (uint32_t)audio_pos;
        }

        /* RMS */
        int64_t sum = 0;
        for (int i = 0; i < audio_pos; i++) {
            int32_t v = audio_buf[i]; sum += v * v;
        }
        int32_t rms = 0;
        { int64_t a = sum / audio_pos; while ((int64_t)rms * rms < a) rms++; }

        /* FM squelch with debounce (inverted: low RMS = signal) */
        if (rms <= SQUELCH_OPEN) { sq_open_count++; sq_close_count = 0; }
        else if (rms > SQUELCH_CLOSE) { sq_close_count++; sq_open_count = 0; }
        else { sq_open_count = 0; sq_close_count = 0; }

        if (!squelch_open && sq_open_count >= SQ_DEBOUNCE) {
            squelch_open = 1;
            open_time = time(NULL);
            detected_ctcss = 0;
            detected_dcs = 0;
            detected_dcs_inv = 0;
            dtmf_pos = 0;
            dtmf_buf[0] = '\0';
            prev_ctcss = 0;
            prev_dcs = 0;
            prev_dtmf = 0;

            /* Don't reset decoders — they've been running continuously
             * and may already have CTCSS/DCS locked from the preamble */

            struct tm *t = localtime(&open_time);
            printf("\n>> %02d:%02d:%02d  SQUELCH OPEN  (RMS %d)\n",
                   t->tm_hour, t->tm_min, t->tm_sec, rms);

        } else if (squelch_open && sq_close_count >= SQ_DEBOUNCE) {
            double dur = difftime(time(NULL), open_time);
            if (dur < 0.1) dur = 0.1;
            dtmf_buf[dtmf_pos] = '\0';

            time_t now = time(NULL);
            struct tm *t = localtime(&now);
            printf(">> %02d:%02d:%02d  SQUELCH CLOSED  (%.1fs", t->tm_hour, t->tm_min, t->tm_sec, dur);
            if (detected_ctcss > 0) printf("  CTCSS=%.1f", detected_ctcss);
            if (detected_dcs > 0) printf("  DCS=%03d%s", detected_dcs, detected_dcs_inv ? "I" : "");
            if (dtmf_pos > 0) printf("  DTMF=%s", dtmf_buf);
            printf(")\n\n");

            squelch_open = 0;

            /* Reset decoders so next transmission gets fresh acquisition */
            plcode_ctcss_dec_reset(ctcss_dec);
            plcode_dcs_dec_reset(dcs_dec);
            plcode_dcs_dec_reset(dcs_dec_inv);
            plcode_dtmf_dec_reset(dtmf_dec);
            detected_ctcss = 0;
            detected_dcs = 0;
            prev_ctcss = 0;
            prev_dcs = 0;
            prev_dtmf = 0;
        }

        /* ── Always feed decoders (even when squelch closed) so they're
         * already locked when squelch opens — DCS PLL needs ~0.5s to lock ── */

        /* CTCSS */
        plcode_ctcss_result_t ctcss_res;
        plcode_ctcss_dec_process(ctcss_dec, audio_buf, (size_t)audio_pos, &ctcss_res);
        if (squelch_open && ctcss_res.detected && !prev_ctcss && ctcss_res.tone_freq_x10 > 0) {
            detected_ctcss = ctcss_res.tone_freq_x10 / 10.0;
            printf("   >> CTCSS: %.1f Hz\n", detected_ctcss);
        } else if (squelch_open && !ctcss_res.detected && prev_ctcss) {
            printf("   >> CTCSS: lost\n");
        }
        prev_ctcss = ctcss_res.detected;

        /* DCS — try both polarities since FM demod sign is ambiguous */
        int16_t *audio_inv = malloc((size_t)audio_pos * sizeof(int16_t));
        for (int i = 0; i < audio_pos; i++) audio_inv[i] = -audio_buf[i];

        plcode_dcs_result_t dcs_res, dcs_res_inv;
        plcode_dcs_dec_process(dcs_dec, audio_buf, (size_t)audio_pos, &dcs_res);
        plcode_dcs_dec_process(dcs_dec_inv, audio_inv, (size_t)audio_pos, &dcs_res_inv);
        free(audio_inv);

        int any_dcs = dcs_res.detected || dcs_res_inv.detected;
        if (squelch_open && any_dcs && !prev_dcs) {
            /* Prefer inverted=0 (non-complement match) — actual transmitted code */
            plcode_dcs_result_t *best;
            if (dcs_res.detected && !dcs_res.inverted)
                best = &dcs_res;
            else if (dcs_res_inv.detected && !dcs_res_inv.inverted)
                best = &dcs_res_inv;
            else
                best = dcs_res.detected ? &dcs_res : &dcs_res_inv;
            detected_dcs = best->code_number;
            detected_dcs_inv = best->inverted;
            printf("   >> DCS: %03d\n", detected_dcs);
        } else if (squelch_open && !any_dcs && prev_dcs) {
            printf("   >> DCS: lost\n");
        }
        prev_dcs = any_dcs;

        /* DTMF */
        plcode_dtmf_result_t dtmf_res;
        plcode_dtmf_dec_process(dtmf_dec, audio_buf, (size_t)audio_pos, &dtmf_res);
        if (squelch_open && dtmf_res.detected && !prev_dtmf) {
            if (dtmf_pos < 63) {
                dtmf_buf[dtmf_pos++] = dtmf_res.digit;
                dtmf_buf[dtmf_pos] = '\0';
                printf("   >> DTMF: %c  (seq: %s)\n", dtmf_res.digit, dtmf_buf);
            }
        }
        prev_dtmf = dtmf_res.detected;

        if (!squelch_open) {
            frame_count++;
            if (frame_count >= 75) {
                frame_count = 0;
                printf("   . idle  RMS=%d\r", rms);
                fflush(stdout);
            }
            continue;
        }

        /* Periodic status line */
        frame_count++;
        if (frame_count >= 15) {
            frame_count = 0;
            int bar_len = (20000 - rms) / 500;
            if (bar_len < 0) bar_len = 0;
            if (bar_len > 40) bar_len = 40;
            char bar[42];
            memset(bar, '#', bar_len);
            bar[bar_len] = '\0';

            double dur = difftime(time(NULL), open_time);
            printf("   %5.1fs  RMS=%-5d %-40s", dur, rms, bar);
            if (detected_ctcss > 0) printf("  PL=%.1f", detected_ctcss);
            if (detected_dcs > 0) printf("  DCS=%03d", detected_dcs);
            if (dtmf_pos > 0) printf("  DTMF=%s", dtmf_buf);
            printf("\n");
        }
    }

    printf("\nShutting down...\n");

    /* Finalize WAV header */
    if (wav_fp) {
        uint32_t data_size = wav_samples * 2;
        uint32_t riff_size = data_size + 36;
        fseek(wav_fp, 4, SEEK_SET);
        fwrite(&riff_size, 4, 1, wav_fp);
        fseek(wav_fp, 40, SEEK_SET);
        fwrite(&data_size, 4, 1, wav_fp);
        fclose(wav_fp);
        printf("Saved %d samples (%.1fs) to sdr_capture.wav\n",
               wav_samples, (double)wav_samples / AUDIO_RATE);
    }

    plcode_ctcss_dec_destroy(ctcss_dec);
    plcode_dcs_dec_destroy(dcs_dec);
    plcode_dcs_dec_destroy(dcs_dec_inv);
    plcode_dtmf_dec_destroy(dtmf_dec);
    rtlsdr_close(dev);
    free(iq_buf);
    free(audio_buf);
    return 0;
}
