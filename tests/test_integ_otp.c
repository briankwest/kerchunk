/*
 * test_integ_otp.c — Integration tests for mod_otp (TOTP authentication)
 */

#include "test_integ_mock.h"
#include "../modules/mod_otp.c"

/* RFC 6238 test vector: secret "12345678901234567890" (ASCII)
 * = Base32 "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ" */
#define TEST_SECRET_B32 "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"

static void setup_test_user_with_totp(void)
{
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "user.1", "name", "TestUser");
    kerchunk_config_set(cfg, "user.1", "ctcss", "1000");
    kerchunk_config_set(cfg, "user.1", "access", "1");
    kerchunk_config_set(cfg, "user.1", "totp_secret", TEST_SECRET_B32);
    kerchunk_user_init(cfg);
    kerchunk_config_destroy(cfg);
}

static void setup_test_user_no_totp(void)
{
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "user.1", "name", "NoTOTP");
    kerchunk_config_set(cfg, "user.1", "ctcss", "1000");
    kerchunk_config_set(cfg, "user.1", "access", "1");
    kerchunk_user_init(cfg);
    kerchunk_config_destroy(cfg);
}

static void init_otp_module(void)
{
    kerchevt_init();
    mock_reset();
    mock_init_core();
    otp_load(&g_mock_core);
    kerchunk_config_t *cfg = kerchunk_config_create();
    kerchunk_config_set(cfg, "otp", "session_timeout", "5000");
    otp_configure(cfg);
    kerchunk_config_destroy(cfg);
}

void test_integ_otp(void)
{
    printf("\n");

    /* ── SHA-1 basic test ── */
    test_begin("otp: SHA-1 of empty string");
    {
        uint8_t digest[20];
        sha1((const uint8_t *)"", 0, digest);
        /* SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709 */
        test_assert(digest[0] == 0xda && digest[1] == 0x39 &&
                    digest[2] == 0xa3 && digest[3] == 0xee,
                    "SHA-1 empty string mismatch");
    }
    test_end();

    test_begin("otp: SHA-1 of 'abc'");
    {
        uint8_t digest[20];
        sha1((const uint8_t *)"abc", 3, digest);
        /* SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d */
        test_assert(digest[0] == 0xa9 && digest[1] == 0x99 &&
                    digest[2] == 0x3e && digest[3] == 0x36,
                    "SHA-1 'abc' mismatch");
    }
    test_end();

    /* ── Base32 decode ── */
    test_begin("otp: Base32 decode known vector");
    {
        uint8_t out[64];
        int len = base32_decode("GEZDGNBVGY3TQOJQ", out, sizeof(out));
        test_assert(len == 10, "expected 10 bytes");
        test_assert(memcmp(out, "1234567890", 10) == 0, "decode mismatch");
    }
    test_end();

    test_begin("otp: Base32 decode case insensitive");
    {
        uint8_t out[64];
        int len = base32_decode("gezdgnbvgy3tqojq", out, sizeof(out));
        test_assert(len == 10, "expected 10 bytes");
        test_assert(memcmp(out, "1234567890", 10) == 0, "decode mismatch");
    }
    test_end();

    /* ── TOTP generation (RFC 6238 test vectors) ── */
    test_begin("otp: TOTP at time=59 matches RFC 6238");
    {
        const uint8_t secret[] = "12345678901234567890";
        int code = totp_generate(secret, 20, 59, 30, 6);
        test_assert(code == 287082, "expected 287082");
    }
    test_end();

    test_begin("otp: TOTP at time=1111111109 matches RFC 6238");
    {
        const uint8_t secret[] = "12345678901234567890";
        int code = totp_generate(secret, 20, 1111111109, 30, 6);
        test_assert(code == 81804, "expected 081804 (81804 as int)");
    }
    test_end();

    /* ── TOTP verification ── */
    test_begin("otp: verify correct code at known time");
    {
        int ok = totp_verify_at(TEST_SECRET_B32, "287082", 59, 0);
        test_assert(ok == 1, "should verify");
    }
    test_end();

    test_begin("otp: reject wrong code");
    {
        int ok = totp_verify_at(TEST_SECRET_B32, "000000", 59, 0);
        test_assert(ok == 0, "should reject");
    }
    test_end();

    test_begin("otp: verify with time skew +1");
    {
        /* Code valid at T=59 (step 1), verify at T=89 (step 2) with skew=1 */
        int ok = totp_verify_at(TEST_SECRET_B32, "287082", 89, 1);
        test_assert(ok == 1, "should verify with skew");
    }
    test_end();

    test_begin("otp: reject beyond skew window");
    {
        /* Code valid at T=59 (step 1), verify at T=120 (step 4) with skew=1 */
        int ok = totp_verify_at(TEST_SECRET_B32, "287082", 120, 1);
        test_assert(ok == 0, "should reject beyond skew");
    }
    test_end();

    /* ── Module integration: successful auth ── */
    test_begin("otp: successful auth starts session");
    setup_test_user_with_totp();
    init_otp_module();
    {
        /* Simulate caller identified */
        kerchevt_t id_evt = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = 1 },
        };
        kerchevt_fire(&id_evt);

        /* Generate correct code for "now" */
        uint8_t secret[64];
        int slen = base32_decode(TEST_SECRET_B32, secret, sizeof(secret));
        int code = totp_generate(secret, (size_t)slen, (uint64_t)time(NULL), 30, 6);
        char code_str[8];
        snprintf(code_str, sizeof(code_str), "%06d", code);

        kerchevt_t otp_evt = {
            .type = DTMF_EVT_OTP_AUTH,
            .custom = { .data = code_str, .len = 6 },
        };
        kerchevt_fire(&otp_evt);

        test_assert(kerchunk_core_get_otp_elevated(1) == 1,
                    "user 1 should be elevated");
    }
    otp_unload();
    kerchevt_shutdown();
    kerchunk_core_set_otp_elevated(1, 0);
    test_end();

    /* ── Module integration: failed auth ── */
    test_begin("otp: failed auth does not elevate");
    setup_test_user_with_totp();
    init_otp_module();
    {
        kerchevt_t id_evt = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = 1 },
        };
        kerchevt_fire(&id_evt);

        kerchevt_t otp_evt = {
            .type = DTMF_EVT_OTP_AUTH,
            .custom = { .data = "000000", .len = 6 },
        };
        kerchevt_fire(&otp_evt);

        test_assert(kerchunk_core_get_otp_elevated(1) == 0,
                    "user 1 should NOT be elevated");
    }
    otp_unload();
    kerchevt_shutdown();
    test_end();

    /* ── No caller ── */
    test_begin("otp: no caller rejects auth");
    setup_test_user_with_totp();
    init_otp_module();
    {
        g_current_caller_id = 0;

        kerchevt_t otp_evt = {
            .type = DTMF_EVT_OTP_AUTH,
            .custom = { .data = "123456", .len = 6 },
        };
        kerchevt_fire(&otp_evt);

        test_assert(kerchunk_core_get_otp_elevated(1) == 0,
                    "should not elevate without caller");
    }
    otp_unload();
    kerchevt_shutdown();
    test_end();

    /* ── Session timer expiry ── */
    test_begin("otp: session timer expires clears elevation");
    setup_test_user_with_totp();
    init_otp_module();
    {
        kerchevt_t id_evt = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = 1 },
        };
        kerchevt_fire(&id_evt);

        uint8_t secret[64];
        int slen = base32_decode(TEST_SECRET_B32, secret, sizeof(secret));
        int code = totp_generate(secret, (size_t)slen, (uint64_t)time(NULL), 30, 6);
        char code_str[8];
        snprintf(code_str, sizeof(code_str), "%06d", code);

        kerchevt_t otp_evt = {
            .type = DTMF_EVT_OTP_AUTH,
            .custom = { .data = code_str, .len = 6 },
        };
        kerchevt_fire(&otp_evt);
        test_assert(kerchunk_core_get_otp_elevated(1) == 1, "should be elevated");

        /* Fire the session timer */
        int timer = mock_find_timer(session_expire_cb);
        test_assert(timer >= 0, "session timer not found");
        mock_fire_timer(timer);

        test_assert(kerchunk_core_get_otp_elevated(1) == 0,
                    "elevation should be cleared after timer");
    }
    otp_unload();
    kerchevt_shutdown();
    test_end();

    /* ── User without TOTP secret ── */
    test_begin("otp: user without TOTP secret rejected");
    setup_test_user_no_totp();
    init_otp_module();
    {
        kerchevt_t id_evt = {
            .type = KERCHEVT_CALLER_IDENTIFIED,
            .caller = { .user_id = 1 },
        };
        kerchevt_fire(&id_evt);

        kerchevt_t otp_evt = {
            .type = DTMF_EVT_OTP_AUTH,
            .custom = { .data = "123456", .len = 6 },
        };
        kerchevt_fire(&otp_evt);

        test_assert(kerchunk_core_get_otp_elevated(1) == 0,
                    "should not elevate without TOTP secret");
    }
    otp_unload();
    kerchevt_shutdown();
    test_end();
}
