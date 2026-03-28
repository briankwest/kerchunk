/*
 * mod_scrambler.c — Frequency inversion voice scrambler
 *
 * Simple frequency inversion voice scrambler.
 * Self-inverse: same operation scrambles and descrambles.
 * Codes 1-8 map to carrier frequencies 2700-3400 Hz (100 Hz steps).
 *
 * DSP: input LPF → NCO mix → output LPF → DC removal → 2x gain
 *
 * Config: [scrambler] section in kerchunk.conf
 * DTMF: *97# toggle, *970# off, *971#-*978# set code
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "../libplcode/src/plcode_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOG_MOD "scrambler"
#define DTMF_EVT_SCRAMBLER (KERCHEVT_CUSTOM + 16)

#define FIR_ORDER  31
#define SAMPLE_RATE 8000

static kerchunk_core_t *g_core;

/* Config */
static int g_enabled;
static int g_code       = 4;     /* 1-8 */
static int g_carrier_hz = 3000;  /* 2700-3400 */

/* CW ID bypass — don't scramble station identification */
static int g_cwid_bypass;

/* FIR coefficients (Q15, shared by all filter instances) */
static int16_t g_fir_coeff[FIR_ORDER];

/* Scrambler state — one for RX (descramble), one for TX (scramble) */
typedef struct {
    uint32_t nco_phase;
    uint32_t nco_inc;
    int16_t  delay_in[FIR_ORDER];
    int16_t  delay_out[FIR_ORDER];
    int      pos_in;
    int      pos_out;
    int32_t  dc_prev_x;
    int32_t  dc_prev_y;
} scrambler_state_t;

static scrambler_state_t g_rx_state;
static scrambler_state_t g_tx_state;

/* ── FIR coefficient computation (Hamming window sinc LPF) ── */

static void compute_fir_coefficients(int carrier_hz)
{
    double fc = (double)carrier_hz / (double)SAMPLE_RATE;
    int M = FIR_ORDER - 1;

    double coeffs_f[FIR_ORDER];
    double sum = 0.0;

    for (int i = 0; i < FIR_ORDER; i++) {
        double n = (double)(i - M / 2);
        double sinc;
        if (fabs(n) < 1e-10)
            sinc = 2.0 * fc;
        else
            sinc = sin(2.0 * M_PI * fc * n) / (M_PI * n);

        /* Hamming window */
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * i / M);
        coeffs_f[i] = sinc * w;
        sum += coeffs_f[i];
    }

    /* Normalize and convert to Q15 */
    for (int i = 0; i < FIR_ORDER; i++)
        g_fir_coeff[i] = (int16_t)((coeffs_f[i] / sum) * 32767.0 + 0.5);
}

/* ── FIR filter (circular buffer, Q15 coefficients) ── */

static inline int16_t fir_process(int16_t *delay, int *pos, int16_t input)
{
    delay[*pos] = input;

    int32_t acc = 0;
    int p = *pos;
    for (int i = 0; i < FIR_ORDER; i++) {
        acc += (int32_t)delay[p] * (int32_t)g_fir_coeff[i];
        p--;
        if (p < 0) p = FIR_ORDER - 1;
    }

    *pos = (*pos + 1) % FIR_ORDER;
    return (int16_t)(acc >> 15);
}

/* ── Init scrambler state ── */

static void state_init(scrambler_state_t *st, int carrier_hz)
{
    memset(st, 0, sizeof(*st));
    st->nco_inc = (uint32_t)((double)carrier_hz / (double)SAMPLE_RATE
                              * 4294967296.0 + 0.5);
}

/* ── Core scrambler processing (in-place, used for both RX and TX) ── */

static void scrambler_process(int16_t *buf, size_t n, void *ctx)
{
    scrambler_state_t *st = (scrambler_state_t *)ctx;

    for (size_t i = 0; i < n; i++) {
        /* 1. Input LPF */
        int16_t filtered = fir_process(st->delay_in, &st->pos_in, buf[i]);

        /* 2. Mix with NCO (frequency shift) */
        int16_t sine = plcode_sine_lookup(st->nco_phase);
        int32_t mixed = ((int32_t)filtered * (int32_t)sine) >> 15;
        st->nco_phase += st->nco_inc;

        /* 3. Output LPF */
        int16_t out = fir_process(st->delay_out, &st->pos_out, (int16_t)mixed);

        /* 4. DC removal: y = x - x_prev + alpha * y_prev (alpha=0.995, Q15=32604) */
        int32_t dc = (int32_t)out - st->dc_prev_x
                     + ((st->dc_prev_y * 32604) >> 15);
        st->dc_prev_x = out;
        st->dc_prev_y = dc;

        /* 5. 2x gain (mixing halves amplitude) + clamp */
        buf[i] = plcode_clamp16(dc * 2);
    }
}

/* ── Wrapper callbacks with bypass logic ── */

static void rx_scrambler_cb(int16_t *buf, size_t n, void *ctx)
{
    /* Skip during emergency mode */
    if (kerchunk_core_get_emergency()) return;
    scrambler_process(buf, n, ctx);
}

static void tx_scrambler_cb(int16_t *buf, size_t n, void *ctx)
{
    /* Skip during emergency mode or CW ID */
    if (kerchunk_core_get_emergency() || g_cwid_bypass) return;
    scrambler_process(buf, n, ctx);
}

/* ── Install/remove core hooks ── */

static void install_hooks(void)
{
    state_init(&g_rx_state, g_carrier_hz);
    state_init(&g_tx_state, g_carrier_hz);
    kerchunk_core_set_rx_scrambler(rx_scrambler_cb, &g_rx_state);
    kerchunk_core_set_tx_scrambler(tx_scrambler_cb, &g_tx_state);
}

static void remove_hooks(void)
{
    kerchunk_core_set_rx_scrambler(NULL, NULL);
    kerchunk_core_set_tx_scrambler(NULL, NULL);
}

/* ── Apply code → carrier conversion ── */

static void apply_code(int code)
{
    if (code < 1) code = 1;
    if (code > 8) code = 8;
    g_code = code;
    g_carrier_hz = 2600 + code * 100;
    compute_fir_coefficients(g_carrier_hz);
}

/* ── Event handlers ── */

static void on_scrambler_cmd(const kerchevt_t *evt, void *ud)
{
    (void)ud;

    /* Extract trailing digits: empty=toggle, 0=off, 1-8=set code */
    int arg = -1;
    if (evt->custom.data && evt->custom.len > 0) {
        char buf[8] = "";
        size_t len = evt->custom.len > 7 ? 7 : evt->custom.len;
        memcpy(buf, evt->custom.data, len);
        buf[len] = '\0';
        arg = atoi(buf);
    }

    if (arg == 0) {
        /* *970# — disable */
        g_enabled = 0;
        remove_hooks();
        if (g_core->tts_speak)
            g_core->tts_speak("Scrambler disabled.", KERCHUNK_PRI_ELEVATED);
    } else if (arg >= 1 && arg <= 8) {
        /* *97N# — set code and enable */
        apply_code(arg);
        g_enabled = 1;
        install_hooks();
        char msg[64];
        snprintf(msg, sizeof(msg), "Scrambler code %d enabled.", g_code);
        if (g_core->tts_speak) g_core->tts_speak(msg, KERCHUNK_PRI_ELEVATED);
    } else {
        /* *97# — toggle */
        g_enabled = !g_enabled;
        if (g_enabled)
            install_hooks();
        else
            remove_hooks();
        if (g_core->tts_speak)
            g_core->tts_speak(g_enabled ? "Scrambler on." : "Scrambler off.", KERCHUNK_PRI_ELEVATED);
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "enabled=%d code=%d carrier=%d Hz",
                g_enabled, g_code, g_carrier_hz);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "scrambler",
                          .description = g_enabled ? "enabled" : "disabled" } };
    kerchevt_fire(&ae);
}

static void on_announcement(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (evt->announcement.source &&
        strcmp(evt->announcement.source, "cwid") == 0)
        g_cwid_bypass = 1;
}

static void on_queue_complete(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_cwid_bypass = 0;
}

/* ── Module lifecycle ── */

static int scrambler_load(kerchunk_core_t *core)
{
    g_core = core;

    if (core->dtmf_register)
        core->dtmf_register("97", 16, "Scrambler toggle", "scrambler_toggle");

    core->subscribe(DTMF_EVT_SCRAMBLER,     on_scrambler_cmd, NULL);
    core->subscribe(KERCHEVT_ANNOUNCEMENT,  on_announcement, NULL);
    core->subscribe(KERCHEVT_QUEUE_COMPLETE, on_queue_complete, NULL);
    return 0;
}

static int scrambler_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "scrambler", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    g_code = kerchunk_config_get_int(cfg, "scrambler", "code", 4);
    apply_code(g_code);

    /* Optional: explicit frequency overrides code */
    int freq = kerchunk_config_get_int(cfg, "scrambler", "frequency", 0);
    if (freq >= 2700 && freq <= 3400) {
        g_carrier_hz = freq;
        compute_fir_coefficients(g_carrier_hz);
    }

    if (g_enabled)
        install_hooks();
    else
        remove_hooks();

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "enabled=%d code=%d carrier=%d Hz",
                g_enabled, g_code, g_carrier_hz);
    return 0;
}

static void scrambler_unload(void)
{
    remove_hooks();
    if (g_core->dtmf_unregister)
        g_core->dtmf_unregister("97");
    g_core->unsubscribe(DTMF_EVT_SCRAMBLER,     on_scrambler_cmd);
    g_core->unsubscribe(KERCHEVT_ANNOUNCEMENT,  on_announcement);
    g_core->unsubscribe(KERCHEVT_QUEUE_COMPLETE, on_queue_complete);
}

/* ── CLI ── */

static int cli_scrambler(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "on") == 0) {
        g_enabled = 1;
        install_hooks();
    } else if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        g_enabled = 0;
        remove_hooks();
    } else if (argc >= 3 && strcmp(argv[1], "code") == 0) {
        int c = atoi(argv[2]);
        if (c >= 1 && c <= 8) {
            apply_code(c);
            if (g_enabled) install_hooks();
        }
    }
    resp_bool(r, "enabled", g_enabled);
    resp_int(r, "code", g_code);
    resp_int(r, "carrier_hz", g_carrier_hz);
    resp_bool(r, "cwid_bypass", g_cwid_bypass);
    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "scrambler", "scrambler [on|off|code <N>]",
      "Voice scrambler status/control", cli_scrambler },
};

static kerchunk_module_def_t mod_scrambler = {
    .name             = "mod_scrambler",
    .version          = "1.0.0",
    .description      = "Frequency inversion voice scrambler",
    .load             = scrambler_load,
    .configure        = scrambler_configure,
    .unload           = scrambler_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_scrambler);
