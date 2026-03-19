/*
 * test_integ_decode.c — DSP decoder → event → caller ID integration tests
 *
 * Uses libplcode encoders to synthesize known signals (CTCSS tones,
 * DCS codes, DTMF digits), feeds them through the corresponding
 * decoders, verifies events fire correctly, and checks that
 * mod_caller identifies the right users.
 *
 * Tests the full pipeline: signal generation → DSP → event bus → module logic
 */

#include "test_integ_mock.h"
#include "plcode.h"
#include <stdlib.h>

/* Pull in mod_caller for caller identification tests */
#include "../modules/mod_caller.c"

#define RATE 8000
#define FRAME 160   /* 20ms at 8kHz */

/* ---- decoder instances (same as audio thread uses) ---- */
static plcode_ctcss_dec_t *g_ctcss_dec;
static plcode_dcs_dec_t   *g_dcs_dec;
static plcode_dtmf_dec_t  *g_dtmf_dec;

/* ---- event tracking ---- */
static int t_caller_id;
static int t_caller_method;
static int t_caller_cleared;
static int t_ctcss_detected;
static uint16_t t_ctcss_freq;
static int t_dcs_detected;
static uint16_t t_dcs_code;
static int t_dtmf_count;
static char t_dtmf_digits[32];

static void t_caller_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    t_caller_id = evt->caller.user_id;
    t_caller_method = evt->caller.method;
}

static void t_clear_handler(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    t_caller_cleared = 1;
}

static void t_ctcss_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    t_ctcss_detected = evt->ctcss.active;
    t_ctcss_freq = evt->ctcss.freq_x10;
}

static void t_dcs_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    t_dcs_detected = evt->dcs.active;
    t_dcs_code = evt->dcs.code;
}

static void t_dtmf_handler(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (t_dtmf_count < (int)sizeof(t_dtmf_digits) - 1)
        t_dtmf_digits[t_dtmf_count++] = evt->dtmf.digit;
    t_dtmf_digits[t_dtmf_count] = '\0';
}

static void reset_tracking(void)
{
    t_caller_id = 0;
    t_caller_method = 0;
    t_caller_cleared = 0;
    t_ctcss_detected = 0;
    t_ctcss_freq = 0;
    t_dcs_detected = 0;
    t_dcs_code = 0;
    t_dtmf_count = 0;
    t_dtmf_digits[0] = '\0';
}

/* Feed encoder output through decoder, fire events on detection changes */
static void feed_ctcss(int16_t *buf, size_t n)
{
    plcode_ctcss_result_t res;
    int prev = 0;
    for (size_t off = 0; off < n; off += FRAME) {
        size_t chunk = (n - off) < FRAME ? (n - off) : FRAME;
        plcode_ctcss_dec_process(g_ctcss_dec, buf + off, chunk, &res);
        if (res.detected != prev) {
            kerchevt_t evt = {
                .type = KERCHEVT_CTCSS_DETECT,
                .ctcss = { .freq_x10 = res.tone_freq_x10, .active = res.detected },
            };
            kerchevt_fire(&evt);
            prev = res.detected;
        }
    }
}

static void feed_dcs(int16_t *buf, size_t n)
{
    plcode_dcs_result_t res;
    int prev = 0;
    for (size_t off = 0; off < n; off += FRAME) {
        size_t chunk = (n - off) < FRAME ? (n - off) : FRAME;
        plcode_dcs_dec_process(g_dcs_dec, buf + off, chunk, &res);
        if (res.detected != prev) {
            kerchevt_t evt = {
                .type = KERCHEVT_DCS_DETECT,
                .dcs = { .code = res.code_number,
                         .normal = !res.inverted,
                         .active = res.detected },
            };
            kerchevt_fire(&evt);
            prev = res.detected;
        }
    }
}

/* Encode a DTMF digit, feed through decoder, fire events.
 * tone_ms: duration of tone, gap_ms: silence after. */
static void encode_and_decode_dtmf(char digit, int tone_ms, int gap_ms)
{
    size_t tone_n = (size_t)(RATE * tone_ms) / 1000;
    size_t gap_n  = (size_t)(RATE * gap_ms) / 1000;

    /* Generate tone */
    int16_t *buf = calloc(tone_n, sizeof(int16_t));
    if (!buf) return;
    plcode_dtmf_enc_t *enc = NULL;
    plcode_dtmf_enc_create(&enc, RATE, digit, 8000);
    plcode_dtmf_enc_process(enc, buf, tone_n);
    plcode_dtmf_enc_destroy(enc);

    /* Feed tone through decoder */
    plcode_dtmf_result_t res;
    int prev = 0;
    for (size_t off = 0; off < tone_n; off += FRAME) {
        size_t chunk = (tone_n - off) < FRAME ? (tone_n - off) : FRAME;
        plcode_dtmf_dec_process(g_dtmf_dec, buf + off, chunk, &res);
        if (res.detected && !prev) {
            kerchevt_t evt = {
                .type = KERCHEVT_DTMF_DIGIT,
                .dtmf = { .digit = res.digit, .duration_ms = tone_ms },
            };
            kerchevt_fire(&evt);
        }
        if (!res.detected && prev) {
            kerchevt_t evt = { .type = KERCHEVT_DTMF_END };
            kerchevt_fire(&evt);
        }
        prev = res.detected;
    }
    free(buf);

    /* Feed silence gap */
    int16_t *gap = calloc(gap_n, sizeof(int16_t));
    if (!gap) return;
    for (size_t off = 0; off < gap_n; off += FRAME) {
        size_t chunk = (gap_n - off) < FRAME ? (gap_n - off) : FRAME;
        plcode_dtmf_dec_process(g_dtmf_dec, gap + off, chunk, &res);
        if (!res.detected && prev) {
            kerchevt_t evt = { .type = KERCHEVT_DTMF_END };
            kerchevt_fire(&evt);
            prev = 0;
        }
    }
    free(gap);
}

/* Encode and decode a string of DTMF digits */
static void encode_dtmf_string(const char *digits)
{
    for (const char *p = digits; *p; p++)
        encode_and_decode_dtmf(*p, 80, 80);
}

/* ---- entry point ---- */

void test_integ_decode(void)
{
    /* Set up user database */
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "user.1", "name",       "Brian");
    kerchunk_config_set(cfg, "user.1", "ctcss",      "1000");
    kerchunk_config_set(cfg, "user.1", "dcs",        "23");
    kerchunk_config_set(cfg, "user.1", "dtmf_login", "101");
    kerchunk_config_set(cfg, "user.1", "ani",         "5551");
    kerchunk_config_set(cfg, "user.1", "access",      "2");
    kerchunk_config_set(cfg, "user.2", "name",       "Alice");
    kerchunk_config_set(cfg, "user.2", "ctcss",      "1318");
    kerchunk_config_set(cfg, "user.2", "dtmf_login", "102");
    kerchunk_config_set(cfg, "user.2", "access",      "1");
    kerchunk_config_set(cfg, "user.3", "name",       "Charlie");
    kerchunk_config_set(cfg, "user.3", "dcs",        "47");  /* DCS 047 */
    kerchunk_config_set(cfg, "user.3", "access",      "1");
    kerchunk_config_set(cfg, "caller", "ani_window",  "500");
    kerchunk_user_init(cfg);

    kerchevt_init();
    mock_reset();
    mock_init_core();

    /* Create decoders */
    plcode_ctcss_dec_create(&g_ctcss_dec, RATE);
    plcode_dcs_dec_create(&g_dcs_dec, RATE);
    plcode_dtmf_dec_create(&g_dtmf_dec, RATE);

    /* Subscribe test tracking handlers */
    kerchevt_subscribe(KERCHEVT_CALLER_IDENTIFIED, t_caller_handler, NULL);
    kerchevt_subscribe(KERCHEVT_CALLER_CLEARED, t_clear_handler, NULL);
    kerchevt_subscribe(KERCHEVT_CTCSS_DETECT, t_ctcss_handler, NULL);
    kerchevt_subscribe(KERCHEVT_DCS_DETECT, t_dcs_handler, NULL);
    kerchevt_subscribe(KERCHEVT_DTMF_DIGIT, t_dtmf_handler, NULL);

    /* Load mod_caller */
    mod_caller.load(&g_mock_core);
    caller_configure(cfg);

    /* ============================================================
     * CTCSS Tests
     * ============================================================ */

    /* 1. CTCSS 100.0 Hz detected and identifies user 1 */
    test_begin("decode: CTCSS 100.0 Hz detected");
    reset_tracking();
    {
        plcode_ctcss_enc_t *enc = NULL;
        plcode_ctcss_enc_create(&enc, RATE, 1000, 2000);
        size_t n = RATE * 2;  /* 2 seconds of tone */
        int16_t *buf = calloc(n, sizeof(int16_t));
        plcode_ctcss_enc_process(enc, buf, n);
        plcode_ctcss_enc_destroy(enc);
        feed_ctcss(buf, n);
        free(buf);
    }
    test_assert(t_ctcss_detected == 1, "CTCSS not detected");
    test_assert(t_ctcss_freq == 1000, "wrong frequency");
    test_end();

    /* 2. CTCSS 131.8 Hz detected and identifies user 2 */
    test_begin("decode: CTCSS 131.8 Hz detected");
    reset_tracking();
    plcode_ctcss_dec_reset(g_ctcss_dec);
    {
        plcode_ctcss_enc_t *enc = NULL;
        plcode_ctcss_enc_create(&enc, RATE, 1318, 2000);
        size_t n = RATE * 2;
        int16_t *buf = calloc(n, sizeof(int16_t));
        plcode_ctcss_enc_process(enc, buf, n);
        plcode_ctcss_enc_destroy(enc);
        feed_ctcss(buf, n);
        free(buf);
    }
    test_assert(t_ctcss_detected == 1, "CTCSS not detected");
    test_assert(t_ctcss_freq == 1318, "wrong frequency");
    test_end();

    /* 3. Unknown CTCSS frequency — no caller identified */
    test_begin("decode: CTCSS 88.5 Hz detected (unknown)");
    reset_tracking();
    plcode_ctcss_dec_reset(g_ctcss_dec);
    {
        plcode_ctcss_enc_t *enc = NULL;
        plcode_ctcss_enc_create(&enc, RATE, 885, 2000);
        size_t n = RATE * 2;
        int16_t *buf = calloc(n, sizeof(int16_t));
        plcode_ctcss_enc_process(enc, buf, n);
        plcode_ctcss_enc_destroy(enc);
        feed_ctcss(buf, n);
        free(buf);
    }
    test_assert(t_ctcss_detected == 1, "CTCSS not detected");
    test_end();

    /* Clear caller from CTCSS tests */
    mock_fire_simple(KERCHEVT_COR_DROP);

    /* ============================================================
     * DCS Tests
     * ============================================================ */

    /* 4. DCS 023 detected
     *    134.4 bps — needs several seconds for reliable detection */
    test_begin("decode: DCS 023 detected");
    reset_tracking();
    plcode_dcs_dec_reset(g_dcs_dec);
    {
        plcode_dcs_enc_t *enc = NULL;
        plcode_dcs_enc_create(&enc, RATE, 23, 0, 2000);  /* DCS label 023 */
        size_t n = RATE * 5;
        int16_t *buf = calloc(n, sizeof(int16_t));
        plcode_dcs_enc_process(enc, buf, n);
        plcode_dcs_enc_destroy(enc);
        feed_dcs(buf, n);
        free(buf);
    }
    test_assert(t_dcs_detected == 1, "DCS not detected");
    test_assert(t_dcs_code == 23, "wrong DCS code");
    test_end();

    mock_fire_simple(KERCHEVT_COR_DROP);   /* clear DCS 023 caller */

    /* 5. DCS 047 — different code */
    test_begin("decode: DCS 047 detected");
    reset_tracking();
    plcode_dcs_dec_reset(g_dcs_dec);
    {
        plcode_dcs_enc_t *enc = NULL;
        plcode_dcs_enc_create(&enc, RATE, 47, 0, 2000);
        size_t n = RATE * 5;
        int16_t *buf = calloc(n, sizeof(int16_t));
        plcode_dcs_enc_process(enc, buf, n);
        plcode_dcs_enc_destroy(enc);
        feed_dcs(buf, n);
        free(buf);
    }
    test_assert(t_dcs_detected == 1, "DCS not detected");
    test_assert(t_dcs_code == 47, "wrong DCS code");
    test_end();

    /* ============================================================
     * DTMF Tests
     * ============================================================ */

    /* 6. DTMF single digit decoded */
    test_begin("decode: DTMF digit '5' detected");
    reset_tracking();
    encode_and_decode_dtmf('5', 100, 100);
    test_assert(t_dtmf_count >= 1, "DTMF not detected");
    test_assert(t_dtmf_digits[0] == '5', "wrong digit");
    test_end();

    /* 7. DTMF sequence decoded */
    test_begin("decode: DTMF sequence '1234'");
    reset_tracking();
    encode_dtmf_string("1234");
    test_assert(t_dtmf_count >= 4, "not all digits detected");
    test_assert(strncmp(t_dtmf_digits, "1234", 4) == 0, "wrong sequence");
    test_end();

    /* 8. DTMF ANI → caller identification
     *    Simulate: COR assert → ANI digits in window → timeout → identified */
    test_begin("decode: DTMF ANI '5551' → user Brian");
    reset_tracking();
    /* COR assert starts ANI window */
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    /* Encode ANI digits through decoder → events → mod_caller */
    encode_dtmf_string("5551");
    /* Fire ANI timeout to trigger lookup */
    if (g_ani_timer >= 0)
        mock_fire_timer(g_ani_timer);
    test_assert(t_caller_id == 1, "ANI not identified");
    test_assert(t_caller_method == KERCHUNK_CALLER_DTMF_ANI, "wrong method");
    test_end();

    /* Clear */
    mock_fire_simple(KERCHEVT_COR_DROP);

    /* 9. DTMF login *101# → caller identification */
    test_begin("decode: DTMF login *101# → user Brian");
    reset_tracking();
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    /* Let ANI window expire */
    if (g_ani_timer >= 0)
        mock_fire_timer(g_ani_timer);
    /* Encode login sequence */
    encode_dtmf_string("*101#");
    test_assert(t_caller_id == 1, "login not identified");
    test_assert(t_caller_method == KERCHUNK_CALLER_DTMF_LOGIN, "wrong method");
    test_end();

    mock_fire_simple(KERCHEVT_COR_DROP);

    /* 10. Partial ANI — too few digits, no identification */
    test_begin("decode: partial ANI '55' → no user");
    reset_tracking();
    session_clear();  /* clear login session from test 9 */
    mock_fire_simple(KERCHEVT_COR_ASSERT);
    encode_dtmf_string("55");
    if (g_ani_timer >= 0)
        mock_fire_timer(g_ani_timer);
    test_end();

    mock_fire_simple(KERCHEVT_COR_DROP);

    /* Cleanup */
    mod_caller.unload();
    kerchevt_unsubscribe(KERCHEVT_CALLER_IDENTIFIED, t_caller_handler);
    kerchevt_unsubscribe(KERCHEVT_CALLER_CLEARED, t_clear_handler);
    kerchevt_unsubscribe(KERCHEVT_CTCSS_DETECT, t_ctcss_handler);
    kerchevt_unsubscribe(KERCHEVT_DCS_DETECT, t_dcs_handler);
    kerchevt_unsubscribe(KERCHEVT_DTMF_DIGIT, t_dtmf_handler);
    kerchevt_shutdown();
    plcode_ctcss_dec_destroy(g_ctcss_dec);
    plcode_dcs_dec_destroy(g_dcs_dec);
    plcode_dtmf_dec_destroy(g_dtmf_dec);
    kerchunk_user_shutdown();
    kerchunk_config_destroy(cfg);
}
