/*
 * mod_parrot.c — Echo/parrot mode
 *
 * User dials *88#, keys up and speaks, releases PTT. The repeater
 * plays back what they said so they can check their audio quality.
 * Max 10 seconds. Recording stops on COR drop.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MOD "parrot"
#define RATE    8000

/* DTMF event offset — *88# */
#define DTMF_EVT_PARROT (KERCHEVT_CUSTOM + 13)

static kerchunk_core_t *g_core;

/* Config */
static int g_enabled       = 1;
static int g_max_duration_s = 10;

/* State (g_recording read by audio thread tap) */
static int             g_armed;
static volatile int    g_recording;
static int16_t *g_buf;
static size_t   g_len;
static size_t   g_cap;

/* Signal quality — accumulated during recording */
static int64_t  g_sq_sum;
static int64_t  g_sq_count;
static int32_t  g_sq_peak_rms;

/* ── Audio tap callback ── */

static void parrot_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_recording || !evt->audio.samples)
        return;

    size_t max_samples = (size_t)RATE * (size_t)g_max_duration_s;
    if (g_len >= max_samples)
        return;

    size_t n = evt->audio.n;
    if (g_len + n > max_samples)
        n = max_samples - g_len;

    if (g_len + n > g_cap) {
        size_t new_cap = g_cap * 2;
        if (new_cap < g_len + n) new_cap = g_len + n + (size_t)RATE;
        if (new_cap > max_samples) new_cap = max_samples;
        int16_t *nb = realloc(g_buf, new_cap * sizeof(int16_t));
        if (!nb) return;
        g_buf = nb;
        g_cap = new_cap;
    }

    memcpy(g_buf + g_len, evt->audio.samples, n * sizeof(int16_t));

    /* Accumulate signal quality stats */
    int64_t frame_sum = 0;
    for (size_t j = 0; j < n; j++) {
        int32_t v = evt->audio.samples[j];
        frame_sum += v * v;
    }
    g_sq_sum += frame_sum;
    g_sq_count += (int64_t)n;
    int32_t frame_rms = 0;
    int64_t avg = frame_sum / (int64_t)n;
    while ((int64_t)frame_rms * frame_rms < avg) frame_rms++;
    if (frame_rms > g_sq_peak_rms) g_sq_peak_rms = frame_rms;

    g_len += n;
}

/* ── Start/stop recording ── */

static void start_recording(void)
{
    if (g_recording) return;

    g_len = 0;
    g_cap = (size_t)RATE * (size_t)g_max_duration_s;
    g_buf = malloc(g_cap * sizeof(int16_t));
    if (!g_buf) return;

    g_sq_sum = 0;
    g_sq_count = 0;
    g_sq_peak_rms = 0;
    g_recording = 1;
    g_core->audio_tap_register(parrot_audio_tap, NULL);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "recording started (max %ds)",
                g_max_duration_s);
}

static void stop_and_playback(void)
{
    if (!g_recording) return;

    g_recording = 0;
    g_core->audio_tap_unregister(parrot_audio_tap);

    if (g_buf && g_len > 0) {
        float dur = (float)g_len / (float)RATE;

        /* Compute average RMS */
        int32_t avg_rms = 0;
        if (g_sq_count > 0) {
            int64_t avg = g_sq_sum / g_sq_count;
            while ((int64_t)avg_rms * avg_rms < avg) avg_rms++;
        }

        /* Convert to percentage of full scale for readability */
        int avg_pct = (int)((avg_rms * 100L) / 32767);
        int peak_pct = (int)((g_sq_peak_rms * 100L) / 32767);

        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "playing back %.1fs (%zu samples) avg_rms=%d(%d%%) peak_rms=%d(%d%%)",
                    dur, g_len, (int)avg_rms, avg_pct,
                    (int)g_sq_peak_rms, peak_pct);

        g_core->queue_silence(200, KERCHUNK_PRI_NORMAL);
        g_core->queue_audio_buffer(g_buf, g_len, KERCHUNK_PRI_NORMAL);

        /* Announce signal quality via TTS */
        if (g_core->tts_speak) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "Audio report. Duration %.1f seconds. "
                     "Average level %d percent. Peak level %d percent.",
                     dur, avg_pct, peak_pct);
            g_core->tts_speak(msg, KERCHUNK_PRI_NORMAL);
        }

        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "parrot", .description = "echo playback" } };
        kerchevt_fire(&ae);
    } else {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "nothing recorded");
    }

    free(g_buf);
    g_buf = NULL;
    g_len = 0;
    g_cap = 0;
}

/* ── Event handlers ── */

static void on_parrot_cmd(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;

    g_armed = 1;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "armed — will record next transmission");
    g_core->queue_tone(1000, 100, 4000, KERCHUNK_PRI_NORMAL);
    g_core->queue_silence(50, KERCHUNK_PRI_NORMAL);
    g_core->queue_tone(1000, 100, 4000, KERCHUNK_PRI_NORMAL);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "parrot", .description = "armed" } };
    kerchevt_fire(&ae);
}

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_armed) return;

    g_armed = 0;
    start_recording();
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_recording)
        stop_and_playback();
}

/* ── Module lifecycle ── */

static int parrot_load(kerchunk_core_t *core)
{
    g_core = core;

    if (core->dtmf_register)
        core->dtmf_register("88", 13, "Parrot echo", "parrot_echo");

    core->subscribe(DTMF_EVT_PARROT,    on_parrot_cmd, NULL);
    core->subscribe(KERCHEVT_COR_ASSERT,  on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,    on_cor_drop, NULL);
    return 0;
}

static int parrot_configure(const kerchunk_config_t *cfg)
{
    const char *v = kerchunk_config_get(cfg, "parrot", "enabled");
    g_enabled = (!v || strcmp(v, "off") != 0);  /* default on */

    g_max_duration_s = kerchunk_config_get_int(cfg, "parrot", "max_duration", 10);
    if (g_max_duration_s > 30) g_max_duration_s = 30;
    if (g_max_duration_s < 1) g_max_duration_s = 1;

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "enabled=%d max_duration=%ds",
                g_enabled, g_max_duration_s);
    return 0;
}

static void parrot_unload(void)
{
    if (g_recording) {
        g_recording = 0;
        g_core->audio_tap_unregister(parrot_audio_tap);
    }
    free(g_buf);
    g_buf = NULL;
    g_armed = 0;
    g_recording = 0;
    g_len = 0;
    g_cap = 0;

    if (g_core->dtmf_unregister)
        g_core->dtmf_unregister("88");
    g_core->unsubscribe(DTMF_EVT_PARROT,    on_parrot_cmd);
    g_core->unsubscribe(KERCHEVT_COR_ASSERT,  on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,    on_cor_drop);
}

/* ── CLI ── */

static int cli_parrot(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    resp_bool(r, "enabled", g_enabled);
    resp_bool(r, "armed", g_armed);
    resp_bool(r, "recording", g_recording);
    resp_int(r, "max_duration_s", g_max_duration_s);
    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "parrot", "parrot", "Parrot/echo status", cli_parrot },
};

static kerchunk_module_def_t mod_parrot = {
    .name             = "mod_parrot",
    .version          = "1.0.0",
    .description      = "Echo/parrot mode for audio quality check",
    .load             = parrot_load,
    .configure        = parrot_configure,
    .unload           = parrot_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_parrot);
