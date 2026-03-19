/*
 * mod_cwid.c — Morse CW callsign identification
 *
 * Uses libplcode's plcode_cwid_enc for Morse generation.
 * 10-minute timer. Queues CW audio during tail or idle.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "plcode.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOG_MOD "cwid"
#define RATE    8000

static kerchunk_core_t *g_core;
static int g_timer_id     = -1;
static int g_pending      = 0;
static int g_cwid_interval_ms = 600000;
static int g_cwid_wpm     = 20;
static int g_cwid_freq    = 800;
static int16_t g_cwid_amp = 4000;
static char g_callsign[16]  = "";
static char g_frequency[16] = "";
static char g_pl_tone[16]   = "";
static int  g_voice_id      = 1;  /* Announce frequency/PL via TTS after CW */

/* Spell a numeric string digit-by-digit for TTS.
 * "462.550" → "four six two point five five zero" */
static void spell_number(const char *num, char *out, size_t outsz)
{
    static const char *words[] = {
        "zero","one","two","three","four",
        "five","six","seven","eight","nine"
    };
    size_t pos = 0;
    for (const char *p = num; *p && pos < outsz - 8; p++) {
        if (pos > 0) { out[pos++] = ' '; }
        if (*p >= '0' && *p <= '9') {
            int n = snprintf(out + pos, outsz - pos, "%s", words[*p - '0']);
            if (n > 0) pos += (size_t)n;
        } else if (*p == '.') {
            int n = snprintf(out + pos, outsz - pos, "point");
            if (n > 0) pos += (size_t)n;
        }
    }
    out[pos] = '\0';
}

static void send_cwid(void)
{
    if (g_callsign[0] == '\0')
        return;

    /* Create CW encoder via libplcode */
    plcode_cwid_enc_t *enc = NULL;
    if (plcode_cwid_enc_create(&enc, RATE, g_callsign,
                                g_cwid_freq, g_cwid_wpm, g_cwid_amp) != PLCODE_OK) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create CW encoder");
        return;
    }

    /* Render into buffer — typical CW ID is 2-5 seconds */
    size_t cap = (size_t)RATE * 10;  /* 10s max (plenty for any callsign) */
    int16_t *buf = calloc(cap, sizeof(int16_t));
    if (!buf) {
        plcode_cwid_enc_destroy(enc);
        return;
    }

    size_t pos = 0;
    while (!plcode_cwid_enc_complete(enc) && pos < cap) {
        size_t chunk = cap - pos;
        if (chunk > 1600) chunk = 1600;  /* 200ms chunks */
        plcode_cwid_enc_process(enc, buf + pos, chunk);
        pos += chunk;
    }
    plcode_cwid_enc_destroy(enc);

    if (pos > 0) {
        g_core->queue_silence(200, 5);
        g_core->queue_audio_buffer(buf, pos, 5);
        g_core->queue_silence(100, 5);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "CW ID queued: %s (%zu samples)",
                    g_callsign, pos);
    }
    free(buf);  /* Always free, even if pos == 0 */

    /* Voice ID: speak frequency and PL tone after CW ID */
    if (g_voice_id && g_core->tts_speak && g_frequency[0]) {
        /* Space out callsign characters so TTS spells it (e.g. "W R D P 5 1 9") */
        char spaced[64];
        size_t si = 0;
        for (const char *p = g_callsign; *p && si < sizeof(spaced) - 2; p++) {
            if (si > 0) spaced[si++] = ' ';
            spaced[si++] = *p;
        }
        spaced[si] = '\0';

        char freq_spoken[128], pl_spoken[128];
        spell_number(g_frequency, freq_spoken, sizeof(freq_spoken));
        spell_number(g_pl_tone, pl_spoken, sizeof(pl_spoken));

        char text[512];
        if (g_pl_tone[0])
            snprintf(text, sizeof(text), "%s repeater, %s megahertz, PL %s hertz.",
                     spaced, freq_spoken, pl_spoken);
        else
            snprintf(text, sizeof(text), "%s repeater, %s megahertz.",
                     spaced, freq_spoken);

        g_core->queue_silence(500, 5);  /* gap between CW and voice */
        g_core->tts_speak(text, 5);
    }

    g_pending = 0;
}

static void cwid_timer_cb(void *ud)
{
    (void)ud;
    if (!g_core->is_receiving() && !g_core->is_transmitting()) {
        send_cwid();
    } else {
        g_pending = 1;
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "CW ID deferred (channel busy)");
    }
}

static void on_tail_start(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_pending)
        send_cwid();
}

static void on_state_change(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (evt->state.new_state == 0 && g_pending) {  /* 0 = RPT_IDLE */
        g_core->request_ptt("cwid");
        send_cwid();
    }
}

static int cwid_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_TAIL_START, on_tail_start, NULL);
    core->subscribe(KERCHEVT_STATE_CHANGE, on_state_change, NULL);
    return 0;
}

static int cwid_configure(const kerchunk_config_t *cfg)
{
    g_cwid_interval_ms = kerchunk_config_get_int(cfg, "repeater", "cwid_interval", 600000);

    /* FCC 95.1751: CW ID must repeat at least every 15 minutes */
    if (g_cwid_interval_ms > 900000) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "cwid_interval capped at 15 min (FCC 95.1751)");
        g_cwid_interval_ms = 900000;
    }

    g_cwid_wpm  = kerchunk_config_get_int(cfg, "repeater", "cwid_wpm", 20);
    if (g_cwid_wpm < 5) g_cwid_wpm = 20;
    g_cwid_freq = kerchunk_config_get_int(cfg, "repeater", "cwid_freq", 800);

    const char *cs = kerchunk_config_get(cfg, "general", "callsign");
    if (cs)
        snprintf(g_callsign, sizeof(g_callsign), "%s", cs);

    const char *freq = kerchunk_config_get(cfg, "general", "frequency");
    if (freq)
        snprintf(g_frequency, sizeof(g_frequency), "%s", freq);

    /* Build PL tone string from repeater default TX CTCSS */
    int tx_ctcss = kerchunk_config_get_int(cfg, "repeater", "tx_ctcss", 0);
    if (tx_ctcss > 0)
        snprintf(g_pl_tone, sizeof(g_pl_tone), "%d.%d",
                 tx_ctcss / 10, tx_ctcss % 10);

    const char *vi = kerchunk_config_get(cfg, "repeater", "voice_id");
    if (vi) g_voice_id = (strcmp(vi, "off") != 0);

    if (g_timer_id >= 0)
        g_core->timer_cancel(g_timer_id);
    g_timer_id = g_core->timer_create(g_cwid_interval_ms, 1, cwid_timer_cb, NULL);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "callsign=%s interval=%dms wpm=%d freq=%d",
                g_callsign, g_cwid_interval_ms, g_cwid_wpm, g_cwid_freq);
    return 0;
}

static void cwid_unload(void)
{
    g_core->unsubscribe(KERCHEVT_TAIL_START, on_tail_start);
    g_core->unsubscribe(KERCHEVT_STATE_CHANGE, on_state_change);
    if (g_timer_id >= 0) {
        g_core->timer_cancel(g_timer_id);
        g_timer_id = -1;
    }
}

static int cli_cwid(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "now") == 0) {
        send_cwid();
        resp_bool(r, "ok", 1);
        resp_str(r, "callsign", g_callsign);
    } else {
        resp_str(r, "callsign", g_callsign);
        resp_bool(r, "pending", g_pending);
        resp_int(r, "interval_s", g_cwid_interval_ms / 1000);
    }
    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "cwid", "cwid [now]", "CW ID status or send now", cli_cwid },
};

static kerchunk_module_def_t mod_cwid = {
    .name         = "mod_cwid",
    .version      = "1.0.0",
    .description  = "CW callsign identification",
    .load         = cwid_load,
    .configure    = cwid_configure,
    .unload       = cwid_unload,
    .cli_commands = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_cwid);
