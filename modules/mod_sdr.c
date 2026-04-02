/*
 * mod_sdr.c — SDR single-channel monitor
 *
 * Tunes to one channel at 240 kHz, FM demod + de-emphasis + decimate to 8 kHz.
 * Identical to sdr_test — proven to produce clean CTCSS/DCS/DTMF detection.
 *
 * Config: [sdr] section in kerchunk.conf
 *   enabled = on
 *   channel = 1          (1-22)
 *   device_index = 0
 *   log_file = sdr_activity.csv
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <plcode.h>
#include <rtl-sdr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#define LOG_MOD "sdr"

#define SAMPLE_RATE  240000
#define AUDIO_RATE   8000
#define DEC_FACTOR   (SAMPLE_RATE / AUDIO_RATE)  /* 30 */
#define IQ_BUFSZ     16384
#define SQUELCH_OPEN  15000
#define SQUELCH_CLOSE 17000
#define SQ_DEBOUNCE   5

/* ── Channel table ── */

#define MAX_CHANNELS 22

typedef struct {
    int    number;
    double freq_hz;
    const char *type;
} sdr_channel_def_t;

static const sdr_channel_def_t g_channel_defs[] = {
    {  1, 462562500, "FRS/GMRS" }, {  2, 462587500, "FRS/GMRS" },
    {  3, 462612500, "FRS/GMRS" }, {  4, 462637500, "FRS/GMRS" },
    {  5, 462662500, "FRS/GMRS" }, {  6, 462687500, "FRS/GMRS" },
    {  7, 462712500, "FRS/GMRS" }, {  8, 467562500, "FRS" },
    {  9, 467587500, "FRS" },      { 10, 467612500, "FRS" },
    { 11, 467637500, "FRS" },      { 12, 467662500, "FRS" },
    { 13, 467687500, "FRS" },      { 14, 467712500, "FRS" },
    { 15, 462550000, "GMRS RPT" }, { 16, 462575000, "GMRS RPT" },
    { 17, 462600000, "GMRS RPT" }, { 18, 462625000, "GMRS RPT" },
    { 19, 462650000, "GMRS RPT" }, { 20, 462675000, "GMRS RPT" },
    { 21, 462700000, "GMRS RPT" }, { 22, 462725000, "GMRS RPT" },
};
#define NUM_CHANNELS (sizeof(g_channel_defs) / sizeof(g_channel_defs[0]))

/* ── Module state ── */

static kerchunk_core_t *g_core;
static int  g_enabled;
static int  g_device_index;
static int  g_channel = 1;
static char g_log_file[256] = "sdr_activity.csv";

static rtlsdr_dev_t *g_dev;
static int g_sdr_tid = -1;

static pthread_mutex_t g_log_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── Activity logging ── */

static void log_activity(int ch_num, double freq, const char *type,
                          double duration, double ctcss, int dcs,
                          const char *dtmf, int avg_rms, int peak_rms)
{
    struct stat st;
    int exists = (stat(g_log_file, &st) == 0 && st.st_size > 0);
    FILE *fp = fopen(g_log_file, "a");
    if (!fp) return;
    if (!exists)
        fprintf(fp, "timestamp,date,time,channel,frequency,type,"
                    "duration_s,ctcss,dcs,dtmf,avg_rms,peak_rms\n");
    time_t now = time(NULL);
    struct tm tbuf;
    struct tm *t = localtime_r(&now, &tbuf);
    pthread_mutex_lock(&g_log_mtx);
    fprintf(fp, "%ld,%04d-%02d-%02d,%02d:%02d:%02d,%d,%.4f,%s,"
                "%.1f,%.1f,%d,%s,%d,%d\n",
            (long)now, t->tm_year+1900, t->tm_mon+1, t->tm_mday,
            t->tm_hour, t->tm_min, t->tm_sec,
            ch_num, freq / 1e6, type,
            duration, ctcss, dcs, dtmf, avg_rms, peak_rms);
    pthread_mutex_unlock(&g_log_mtx);
    fclose(fp);
}

/* ── Monitor thread (matches sdr_test exactly) ── */

static void *monitor_thread(void *arg)
{
    (void)arg;

    /* Find channel definition */
    const sdr_channel_def_t *chdef = NULL;
    for (int i = 0; i < (int)NUM_CHANNELS; i++) {
        if (g_channel_defs[i].number == g_channel) {
            chdef = &g_channel_defs[i]; break;
        }
    }
    if (!chdef) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "channel %d not found", g_channel);
        return NULL;
    }

    /* Configure device — exactly like sdr_test */
    rtlsdr_set_center_freq(g_dev, (uint32_t)chdef->freq_hz);
    rtlsdr_set_sample_rate(g_dev, SAMPLE_RATE);
    rtlsdr_set_tuner_gain_mode(g_dev, 0);
    rtlsdr_reset_buffer(g_dev);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "monitoring ch %d (%.4f MHz %s) @ %d Hz",
                chdef->number, chdef->freq_hz / 1e6, chdef->type, SAMPLE_RATE);

    /* Create decoders */
    plcode_ctcss_dec_t *ctcss_dec = NULL;
    plcode_dcs_dec_t   *dcs_dec   = NULL;
    plcode_dcs_dec_t   *dcs_dec_inv = NULL;
    plcode_dtmf_dec_t  *dtmf_dec  = NULL;
    plcode_ctcss_dec_create(&ctcss_dec, AUDIO_RATE);
    plcode_dcs_dec_create(&dcs_dec, AUDIO_RATE);
    plcode_dcs_dec_create(&dcs_dec_inv, AUDIO_RATE);
    plcode_dtmf_dec_create(&dtmf_dec, AUDIO_RATE);

    unsigned char *iq_buf = malloc(IQ_BUFSZ);
    int16_t *audio_buf = malloc(IQ_BUFSZ * sizeof(int16_t));
    int16_t *audio_inv = malloc(IQ_BUFSZ * sizeof(int16_t));
    if (!iq_buf || !audio_buf || !audio_inv) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to allocate SDR buffers");
        free(iq_buf);
        free(audio_buf);
        free(audio_inv);
        return NULL;
    }

    float prev_i = 0, prev_q = 0;
    float deemph = 0;
    float dec_acc = 0;
    int dec_count = 0;

    int squelch_open = 0, sq_open_count = 0, sq_close_count = 0;
    double detected_ctcss = 0;
    int detected_dcs = 0;
    char dtmf_buf[64] = {0};
    int dtmf_pos = 0;
    int prev_ctcss = 0, prev_dcs = 0, prev_dtmf = 0;
    time_t open_time = 0;
    int64_t rms_sum = 0;
    int rms_count = 0;
    int32_t peak_rms = 0;

    while (!g_core->thread_should_stop(g_sdr_tid)) {
        int n_read = 0;
        if (rtlsdr_read_sync(g_dev, iq_buf, IQ_BUFSZ, &n_read) < 0) break;
        if (n_read == 0) continue;

        int nsamples = n_read / 2;
        int audio_pos = 0;

        /* IQ -> FM demod -> de-emphasis -> decimate to 8kHz */
        for (int i = 0; i < nsamples; i++) {
            float si = ((float)iq_buf[i * 2]     - 127.5f) / 127.5f;
            float sq = ((float)iq_buf[i * 2 + 1] - 127.5f) / 127.5f;

            float dot   = si * prev_i + sq * prev_q;
            float cross = sq * prev_i - si * prev_q;
            float fm = atan2f(cross, dot);
            prev_i = si; prev_q = sq;

            float audio = fm * (16000.0f / 0.07f);
            float alpha = 1.0f / (1.0f + (float)SAMPLE_RATE * 75e-6f
                                  * 2.0f * (float)M_PI);
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

        /* RMS */
        int64_t sum = 0;
        for (int i = 0; i < audio_pos; i++) {
            int32_t v = audio_buf[i]; sum += v * v;
        }
        int32_t rms = 0;
        { int64_t a = sum / audio_pos; while ((int64_t)rms * rms < a) rms++; }

        /* Always feed decoders so they're locked when squelch opens */
        plcode_ctcss_result_t ctcss_res;
        plcode_ctcss_dec_process(ctcss_dec, audio_buf, (size_t)audio_pos, &ctcss_res);

        for (int i = 0; i < audio_pos; i++) audio_inv[i] = -audio_buf[i];
        plcode_dcs_result_t dcs_res, dcs_res_inv;
        plcode_dcs_dec_process(dcs_dec, audio_buf, (size_t)audio_pos, &dcs_res);
        plcode_dcs_dec_process(dcs_dec_inv, audio_inv, (size_t)audio_pos, &dcs_res_inv);

        plcode_dtmf_result_t dtmf_res;
        plcode_dtmf_dec_process(dtmf_dec, audio_buf, (size_t)audio_pos, &dtmf_res);

        /* FM-inverted squelch with debounce */
        if (rms <= SQUELCH_OPEN) { sq_open_count++; sq_close_count = 0; }
        else if (rms > SQUELCH_CLOSE) { sq_close_count++; sq_open_count = 0; }
        else { sq_open_count = 0; sq_close_count = 0; }

        if (!squelch_open && sq_open_count >= SQ_DEBOUNCE) {
            squelch_open = 1;
            open_time = time(NULL);
            detected_ctcss = 0;
            detected_dcs = 0;
            dtmf_pos = 0;
            dtmf_buf[0] = '\0';
            prev_ctcss = 0;
            prev_dcs = 0;
            prev_dtmf = 0;
            rms_sum = 0;
            rms_count = 0;
            peak_rms = 0;

            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "ch %d (%.4f MHz %s) active (RMS %d)",
                        chdef->number, chdef->freq_hz / 1e6,
                        chdef->type, rms);

        } else if (squelch_open && sq_close_count >= SQ_DEBOUNCE) {
            double dur = difftime(time(NULL), open_time);
            if (dur < 0.1) dur = 0.1;
            dtmf_buf[dtmf_pos] = '\0';

            int32_t avg_rms = 0;
            if (rms_count > 0) {
                int64_t a = rms_sum / rms_count;
                while ((int64_t)avg_rms * avg_rms < a) avg_rms++;
            }

            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "ch %d idle (%.1fs ctcss=%.1f dcs=%d dtmf=%s rms=%d/%d)",
                        chdef->number, dur,
                        detected_ctcss, detected_dcs,
                        dtmf_pos > 0 ? dtmf_buf : "-",
                        (int)avg_rms, (int)peak_rms);

            log_activity(chdef->number, chdef->freq_hz, chdef->type,
                         dur, detected_ctcss, detected_dcs,
                         dtmf_buf, (int)avg_rms, (int)peak_rms);

            squelch_open = 0;

            /* Reset decoders for next transmission */
            plcode_ctcss_dec_reset(ctcss_dec);
            plcode_dcs_dec_reset(dcs_dec);
            plcode_dcs_dec_reset(dcs_dec_inv);
            plcode_dtmf_dec_reset(dtmf_dec);
        }

        if (!squelch_open) continue;

        /* Accumulate signal quality */
        rms_sum += sum;
        rms_count += audio_pos;
        if (rms > peak_rms) peak_rms = rms;

        /* Report detections */
        if (ctcss_res.detected && !prev_ctcss && ctcss_res.tone_freq_x10 > 0) {
            detected_ctcss = ctcss_res.tone_freq_x10 / 10.0;
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "ch %d CTCSS: %.1f Hz", chdef->number, detected_ctcss);
        }
        prev_ctcss = ctcss_res.detected;

        int any_dcs = dcs_res.detected || dcs_res_inv.detected;
        if (any_dcs && !prev_dcs) {
            plcode_dcs_result_t *best;
            if (dcs_res.detected && !dcs_res.inverted)
                best = &dcs_res;
            else if (dcs_res_inv.detected && !dcs_res_inv.inverted)
                best = &dcs_res_inv;
            else
                best = dcs_res.detected ? &dcs_res : &dcs_res_inv;
            detected_dcs = best->code_number;
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "ch %d DCS: %03d", chdef->number, detected_dcs);
        }
        prev_dcs = any_dcs;

        if (dtmf_res.detected && !prev_dtmf && dtmf_pos < 63) {
            dtmf_buf[dtmf_pos++] = dtmf_res.digit;
            dtmf_buf[dtmf_pos] = '\0';
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "ch %d DTMF: %c (seq: %s)",
                        chdef->number, dtmf_res.digit, dtmf_buf);
        }
        prev_dtmf = dtmf_res.detected;
    }

    plcode_ctcss_dec_destroy(ctcss_dec);
    plcode_dcs_dec_destroy(dcs_dec);
    plcode_dcs_dec_destroy(dcs_dec_inv);
    plcode_dtmf_dec_destroy(dtmf_dec);
    free(iq_buf);
    free(audio_buf);
    free(audio_inv);
    return NULL;
}

/* ── Module lifecycle ── */

static int sdr_load(kerchunk_core_t *core)
{
    g_core = core;
    return 0;
}

static int sdr_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "sdr", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    g_device_index = kerchunk_config_get_int(cfg, "sdr", "device_index", 0);
    g_channel = kerchunk_config_get_int(cfg, "sdr", "channel", 1);

    v = kerchunk_config_get(cfg, "sdr", "log_file");
    if (v) snprintf(g_log_file, sizeof(g_log_file), "%s", v);

    if (g_sdr_tid >= 0) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "config reloaded (running)");
        return 0;
    }

    if (!g_enabled) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled");
        return 0;
    }

    int count = rtlsdr_get_device_count();
    if (count == 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "no RTL-SDR devices found");
        return 0;
    }

    int rc = rtlsdr_open(&g_dev, (uint32_t)g_device_index);
    if (rc < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "failed to open device %d (rc=%d)", g_device_index, rc);
        return 0;
    }

    const char *name = rtlsdr_get_device_name((uint32_t)g_device_index);

    g_sdr_tid = g_core->thread_create("sdr-monitor", monitor_thread, NULL);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "started: device=%s channel=%d",
                name ? name : "unknown", g_channel);

    return 0;
}

static void sdr_unload(void)
{
    if (g_sdr_tid >= 0) {
        g_core->thread_stop(g_sdr_tid);
        if (g_dev) { rtlsdr_close(g_dev); g_dev = NULL; }
        g_core->thread_join(g_sdr_tid);
        g_sdr_tid = -1;
    }
}

/* ── CLI ── */

static int cli_sdr(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    resp_bool(r, "enabled", g_enabled);
    resp_bool(r, "running", g_sdr_tid >= 0);
    resp_int(r, "channel", g_channel);
    return 0;

usage:
    resp_text_raw(r, "SDR single-channel monitor (RTL-SDR)\n\n"
        "  sdr\n"
        "    Show SDR monitor status.\n\n"
        "    Fields:\n"
        "      enabled    Whether SDR monitoring is configured on\n"
        "      running    Whether the monitor thread is active\n"
        "      channel    FRS/GMRS channel number being monitored (1-22)\n\n"
        "    Tunes an RTL-SDR dongle to one FRS/GMRS channel at 240 kHz.\n"
        "    Performs FM demod, de-emphasis, decimation to 8 kHz, and\n"
        "    CTCSS/DCS/DTMF detection. Activity is logged to CSV.\n\n"
        "Config: [sdr] enabled, channel, device_index, log_file\n");
    resp_str(r, "error", "usage: sdr [help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "sdr", "sdr", "SDR monitor status", cli_sdr },
};

static kerchunk_module_def_t mod_sdr = {
    .name             = "mod_sdr",
    .version          = "1.0.0",
    .description      = "SDR channel monitor",
    .load             = sdr_load,
    .configure        = sdr_configure,
    .unload           = sdr_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_sdr);
