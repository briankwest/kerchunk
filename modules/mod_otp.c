/*
 * mod_otp.c — TOTP authentication for elevated access
 *
 * RFC 6238 TOTP with embedded SHA-1/HMAC-SHA1 (no external crypto deps).
 * Users dial *68<6 digits># to authenticate via Google Authenticator.
 * Grants time-limited elevated access for privileged DTMF commands.
 *
 * Config: [otp] section in kerchunk.conf
 * User config: totp_secret = <base32 key> in [user.N]
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define LOG_MOD "otp"
#define DTMF_EVT_OTP_AUTH   (KERCHEVT_CUSTOM + 15)
#define MAX_OTP_SESSIONS    64

static kerchunk_core_t *g_core;
static pthread_mutex_t g_otp_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Config */
static int g_session_timeout_ms = 120000;  /* 2 minutes */
static int g_time_skew          = 1;       /* +/- time steps */

/* Last identified caller. Persists across COR drops to avoid the race
 * with mod_dtmfcmd's deferred dispatch — see mod_voicemail.c for the
 * full explanation. mod_caller fires CALLER_CLEARED on every COR drop
 * for login sessions even though the session itself stays alive, so
 * tracking the last identified user (instead of the transient
 * "actively transmitting" id) is the only way OTP commands dispatched
 * on COR drop can still see who they belong to. */
static int g_current_caller_id;

/* Brute-force lockout */
#define OTP_MAX_FAILURES 5
#define OTP_LOCKOUT_S 300  /* 5 minutes */

static struct {
    int    user_id;
    int    failures;
    time_t last_failure;
} g_otp_lockout[32];

/* Replay guard: remember the highest TOTP counter we've already accepted
 * for each user, so a captured code isn't usable a second time within
 * its own validity window (or within the +/- skew grace we permit). */
static struct {
    int      user_id;
    uint64_t last_counter;
} g_otp_last_counter[32];

/* Session table */
typedef struct {
    int user_id;
    int active;
    int timer_id;
} otp_session_t;

static otp_session_t g_sessions[MAX_OTP_SESSIONS];

/* ── SHA-1 (FIPS 180-4) ── */

static uint32_t sha1_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | block[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;             k = 0xCA62C1D6; }
        uint32_t t = sha1_rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = sha1_rotl(b, 30); b = a; a = t;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t state[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t i;
    for (i = 0; i + 64 <= len; i += 64)
        sha1_transform(state, data + i);

    uint8_t block[64];
    size_t rem = len - i;
    memcpy(block, data + i, rem);
    block[rem++] = 0x80;
    if (rem > 56) {
        memset(block + rem, 0, 64 - rem);
        sha1_transform(state, block);
        rem = 0;
    }
    memset(block + rem, 0, 56 - rem);
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++)
        block[56 + j] = (uint8_t)(bits >> (56 - j * 8));
    sha1_transform(state, block);

    for (int j = 0; j < 5; j++) {
        out[j*4]   = (uint8_t)(state[j] >> 24);
        out[j*4+1] = (uint8_t)(state[j] >> 16);
        out[j*4+2] = (uint8_t)(state[j] >> 8);
        out[j*4+3] = (uint8_t)(state[j]);
    }
}

/* ── HMAC-SHA1 (RFC 2104) ── */

static void hmac_sha1(const uint8_t *key, size_t klen,
                       const uint8_t *msg, size_t mlen,
                       uint8_t out[20])
{
    uint8_t k_pad[64];
    if (klen > 64) {
        sha1(key, klen, k_pad);
        memset(k_pad + 20, 0, 44);
    } else {
        memcpy(k_pad, key, klen);
        memset(k_pad + klen, 0, 64 - klen);
    }

    /* inner hash: SHA1((K XOR ipad) || msg) */
    uint8_t *inner = malloc(64 + mlen);
    if (!inner) { memset(out, 0, 20); return; }
    for (int i = 0; i < 64; i++) inner[i] = k_pad[i] ^ 0x36;
    memcpy(inner + 64, msg, mlen);
    uint8_t inner_hash[20];
    sha1(inner, 64 + mlen, inner_hash);
    free(inner);

    /* outer hash: SHA1((K XOR opad) || inner_hash) */
    uint8_t outer[84];
    for (int i = 0; i < 64; i++) outer[i] = k_pad[i] ^ 0x5C;
    memcpy(outer + 64, inner_hash, 20);
    sha1(outer, 84, out);
}

/* ── Base32 decode (RFC 4648) ── */

static int base32_decode(const char *encoded, uint8_t *out, size_t out_max)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    int buffer = 0, bits_left = 0;
    size_t count = 0;

    for (; *encoded && *encoded != '='; encoded++) {
        char c = *encoded;
        if (c >= 'a' && c <= 'z') c -= 32;
        const char *p = strchr(alphabet, c);
        if (!p) continue;
        buffer = (buffer << 5) | (int)(p - alphabet);
        bits_left += 5;
        if (bits_left >= 8) {
            if (count >= out_max) return (int)count;
            out[count++] = (uint8_t)(buffer >> (bits_left - 8));
            bits_left -= 8;
        }
    }
    return (int)count;
}

/* ── TOTP (RFC 6238) ── */

static int totp_generate(const uint8_t *secret, size_t secret_len,
                          uint64_t time_val, int step, int digits)
{
    uint64_t counter = time_val / (uint64_t)step;
    uint8_t msg[8];
    for (int i = 7; i >= 0; i--) {
        msg[i] = (uint8_t)(counter & 0xFF);
        counter >>= 8;
    }

    uint8_t hmac[20];
    hmac_sha1(secret, secret_len, msg, 8, hmac);

    int offset = hmac[19] & 0x0F;
    uint32_t code = ((uint32_t)(hmac[offset] & 0x7F) << 24) |
                    ((uint32_t)hmac[offset+1] << 16) |
                    ((uint32_t)hmac[offset+2] << 8) |
                    (uint32_t)hmac[offset+3];

    int mod = 1;
    for (int i = 0; i < digits; i++) mod *= 10;
    return (int)(code % (uint32_t)mod);
}

static int totp_verify_at(const char *base32_secret, const char *code_str,
                           uint64_t now, int skew, uint64_t *matched_counter)
{
    uint8_t secret[64];
    int secret_len = base32_decode(base32_secret, secret, sizeof(secret));
    if (secret_len <= 0) return 0;

    int code = atoi(code_str);

    for (int i = -skew; i <= skew; i++) {
        uint64_t t = now + (int64_t)i * 30;
        int expected = totp_generate(secret, (size_t)secret_len, t, 30, 6);
        if (code == expected) {
            if (matched_counter) *matched_counter = t / 30;
            return 1;
        }
    }
    return 0;
}

static int totp_verify(const char *base32_secret, const char *code_str, int skew,
                        uint64_t *matched_counter)
{
    return totp_verify_at(base32_secret, code_str, (uint64_t)time(NULL),
                           skew, matched_counter);
}

/* ── Session management ── */

static otp_session_t *find_session(int user_id)
{
    for (int i = 0; i < MAX_OTP_SESSIONS; i++)
        if (g_sessions[i].active && g_sessions[i].user_id == user_id)
            return &g_sessions[i];
    return NULL;
}

static void session_expire_cb(void *ud)
{
    otp_session_t *s = (otp_session_t *)ud;
    pthread_mutex_lock(&g_otp_mutex);
    if (!s || !s->active) { pthread_mutex_unlock(&g_otp_mutex); return; }

    int user_id = s->user_id;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "session expired for user %d", user_id);
    kerchunk_core_set_otp_elevated(user_id, 0);
    s->active = 0;
    s->timer_id = -1;
    pthread_mutex_unlock(&g_otp_mutex);

    if (g_core->tts_speak)
        g_core->tts_speak("Elevated session expired.", KERCHUNK_PRI_ELEVATED);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "otp", .description = "session expired" } };
    kerchevt_fire(&ae);
}

static void session_start(int user_id)
{
    /* Reuse existing or find free slot */
    otp_session_t *s = find_session(user_id);
    if (s) {
        /* Already active — reset timer */
        if (s->timer_id >= 0)
            g_core->timer_cancel(s->timer_id);
    } else {
        for (int i = 0; i < MAX_OTP_SESSIONS; i++) {
            if (!g_sessions[i].active) {
                s = &g_sessions[i];
                break;
            }
        }
        if (!s) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "no session slots available");
            return;
        }
    }

    s->user_id = user_id;
    s->active = 1;
    s->timer_id = g_core->timer_create(g_session_timeout_ms, 0,
                                         session_expire_cb, s);
    kerchunk_core_set_otp_elevated(user_id, 1);

    const kerchunk_user_t *u = g_core->user_lookup_by_id(user_id);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "elevated session started: %s (id=%d, timeout=%ds)",
                u ? u->name : "unknown", user_id,
                g_session_timeout_ms / 1000);
}

/* ── Event handlers ── */

static void on_otp_command(const kerchevt_t *evt, void *ud)
{
    (void)ud;

    pthread_mutex_lock(&g_otp_mutex);
    int caller_id = g_current_caller_id;
    pthread_mutex_unlock(&g_otp_mutex);

    if (caller_id <= 0) {
        if (g_core->tts_speak)
            g_core->tts_speak("Access denied. Please identify first.", KERCHUNK_PRI_ELEVATED);
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "OTP attempt with no caller ID");
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "otp", .description = "access denied" } };
        kerchevt_fire(&ae);
        return;
    }

    const kerchunk_user_t *u = g_core->user_lookup_by_id(caller_id);
    if (!u || u->totp_secret[0] == '\0') {
        if (g_core->tts_speak)
            g_core->tts_speak("OTP not configured for this user.", KERCHUNK_PRI_ELEVATED);
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "OTP attempt by user %d with no TOTP secret", caller_id);
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "otp", .description = "not configured" } };
        kerchevt_fire(&ae);
        return;
    }

    /* Extract code from event data */
    if (!evt->custom.data || evt->custom.len < 6) {
        if (g_core->tts_speak)
            g_core->tts_speak("Invalid code.", KERCHUNK_PRI_ELEVATED);
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "otp", .description = "invalid code" } };
        kerchevt_fire(&ae);
        return;
    }

    char code[8] = "";
    size_t len = evt->custom.len > 6 ? 6 : evt->custom.len;
    memcpy(code, evt->custom.data, len);
    code[len] = '\0';

    /* Verify only digits */
    for (size_t i = 0; i < len; i++) {
        if (code[i] < '0' || code[i] > '9') {
            if (g_core->tts_speak)
                g_core->tts_speak("Invalid code.", KERCHUNK_PRI_ELEVATED);
            kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
                .announcement = { .source = "otp", .description = "invalid code" } };
            kerchevt_fire(&ae);
            return;
        }
    }

    /* Check lockout */
    {
        time_t now = time(NULL);
        for (int i = 0; i < 32; i++) {
            if (g_otp_lockout[i].user_id == caller_id) {
                if (g_otp_lockout[i].failures >= OTP_MAX_FAILURES &&
                    now - g_otp_lockout[i].last_failure < OTP_LOCKOUT_S) {
                    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                                "OTP locked out for user %d (%d failures)",
                                caller_id, g_otp_lockout[i].failures);
                    return;  /* silently ignore — don't reveal lockout to attacker */
                }
                if (now - g_otp_lockout[i].last_failure >= OTP_LOCKOUT_S)
                    g_otp_lockout[i].failures = 0;  /* reset after lockout expires */
                break;
            }
        }
    }

    uint64_t matched_counter = 0;
    if (totp_verify(u->totp_secret, code, g_time_skew, &matched_counter)) {
        /* Replay guard: refuse a code whose counter we've already accepted.
         * Without this, a captured code is usable again for the rest of its
         * 30-second window (and up to +/- g_time_skew windows of grace). */
        int replay_slot = -1;
        int replay = 0;
        for (int i = 0; i < 32; i++) {
            if (g_otp_last_counter[i].user_id == caller_id) {
                replay_slot = i;
                if (matched_counter <= g_otp_last_counter[i].last_counter)
                    replay = 1;
                break;
            }
            if (replay_slot < 0 && g_otp_last_counter[i].user_id == 0)
                replay_slot = i;
        }
        if (replay) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "OTP replay rejected for %s (id=%d, counter=%llu)",
                        u->name, u->id,
                        (unsigned long long)matched_counter);
            if (g_core->tts_speak)
                g_core->tts_speak("Authentication failed.", KERCHUNK_PRI_ELEVATED);
            g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_ELEVATED);
            kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
                .announcement = { .source = "otp", .description = "replay rejected" } };
            kerchevt_fire(&ae);
            return;
        }
        /* Record the counter so future replays within the skew window fail. */
        if (replay_slot >= 0) {
            g_otp_last_counter[replay_slot].user_id      = caller_id;
            g_otp_last_counter[replay_slot].last_counter = matched_counter;
        }

        /* Clear lockout on success */
        for (int i = 0; i < 32; i++) {
            if (g_otp_lockout[i].user_id == caller_id) {
                g_otp_lockout[i].failures = 0;
                break;
            }
        }
        pthread_mutex_lock(&g_otp_mutex);
        session_start(caller_id);
        pthread_mutex_unlock(&g_otp_mutex);
        if (g_core->tts_speak)
            g_core->tts_speak("Authentication successful. Elevated access granted.", KERCHUNK_PRI_ELEVATED);
        g_core->queue_tone(800, 200, 4000, KERCHUNK_PRI_ELEVATED);
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "otp", .description = "auth success" } };
        kerchevt_fire(&ae);
    } else {
        /* Record failure */
        {
            int slot = -1;
            for (int i = 0; i < 32; i++) {
                if (g_otp_lockout[i].user_id == caller_id) { slot = i; break; }
                if (g_otp_lockout[i].user_id == 0) { slot = i; break; }
            }
            if (slot >= 0) {
                g_otp_lockout[slot].user_id = caller_id;
                g_otp_lockout[slot].failures++;
                g_otp_lockout[slot].last_failure = time(NULL);
            }
        }
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "OTP failed for %s (id=%d)", u->name, u->id);
        if (g_core->tts_speak)
            g_core->tts_speak("Authentication failed.", KERCHUNK_PRI_ELEVATED);
        g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_ELEVATED);
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "otp", .description = "auth failed" } };
        kerchevt_fire(&ae);
    }
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    pthread_mutex_lock(&g_otp_mutex);
    g_current_caller_id = evt->caller.user_id;
    pthread_mutex_unlock(&g_otp_mutex);
}

/* Note: we deliberately do NOT subscribe to KERCHEVT_CALLER_CLEARED.
 * mod_caller fires CALLER_CLEARED on every COR drop for login sessions,
 * even though the session itself persists. Clearing g_current_caller_id
 * here would race with the deferred DTMF command dispatch (also on
 * COR_DROP) and cause every *68# command to land with caller_id == 0,
 * speaking "Access denied. Please identify first." even when the user
 * is logged in. Same race as mod_voicemail. */

/* ── Module lifecycle ── */

static int otp_load(kerchunk_core_t *core)
{
    g_core = core;
    memset(g_sessions, 0, sizeof(g_sessions));
    for (int i = 0; i < MAX_OTP_SESSIONS; i++)
        g_sessions[i].timer_id = -1;
    /* Reset runtime-only state (lockout counters, replay guard) on load.
     * Both are in-memory and wouldn't survive a daemon restart anyway —
     * a module reload is equivalent from the operator's perspective. */
    memset(g_otp_lockout, 0, sizeof(g_otp_lockout));
    memset(g_otp_last_counter, 0, sizeof(g_otp_last_counter));

    if (core->dtmf_register)
        core->dtmf_register("68", 15, "OTP authenticate", "otp_auth");

    core->subscribe(DTMF_EVT_OTP_AUTH, on_otp_command, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    return 0;
}

static int otp_configure(const kerchunk_config_t *cfg)
{
    g_session_timeout_ms = kerchunk_config_get_duration_ms(cfg, "otp", "session_timeout", 120000);
    g_time_skew = kerchunk_config_get_int(cfg, "otp", "time_skew", 1);

    /* Count users with TOTP secrets */
    int count = 0;
    int total = kerchunk_user_count();
    for (int i = 0; i < total; i++) {
        const kerchunk_user_t *u = kerchunk_user_get(i);
        if (u && u->totp_secret[0]) count++;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "session_timeout=%ds time_skew=%d users_with_totp=%d",
                g_session_timeout_ms / 1000, g_time_skew, count);
    return 0;
}

static void otp_unload(void)
{
    for (int i = 0; i < MAX_OTP_SESSIONS; i++) {
        if (g_sessions[i].active) {
            if (g_sessions[i].timer_id >= 0)
                g_core->timer_cancel(g_sessions[i].timer_id);
            kerchunk_core_set_otp_elevated(g_sessions[i].user_id, 0);
        }
    }
    if (g_core->dtmf_unregister)
        g_core->dtmf_unregister("68");
    g_core->unsubscribe(DTMF_EVT_OTP_AUTH, on_otp_command);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
}

/* ── CLI ── */

static int cli_otp(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    resp_int(r, "session_timeout_s", g_session_timeout_ms / 1000);
    resp_int(r, "time_skew", g_time_skew);

    int active = 0;
    for (int i = 0; i < MAX_OTP_SESSIONS; i++)
        if (g_sessions[i].active) active++;
    resp_int(r, "active_sessions", active);

    /* List active sessions */
    if (active > 0) {
        resp_json_raw(r, ",\"sessions\":[");
        int first = 1;
        for (int i = 0; i < MAX_OTP_SESSIONS; i++) {
            if (!g_sessions[i].active) continue;
            const kerchunk_user_t *u = g_core->user_lookup_by_id(g_sessions[i].user_id);
            if (!first) resp_json_raw(r, ",");
            char frag[128];
            snprintf(frag, sizeof(frag), "{\"user_id\":%d,\"name\":\"%s\"}",
                     g_sessions[i].user_id, u ? u->name : "unknown");
            resp_json_raw(r, frag);
            first = 0;
        }
        resp_json_raw(r, "]");
    }

    return 0;

usage:
    resp_text_raw(r, "TOTP authentication for elevated access\n\n"
        "  otp\n"
        "    Show OTP session status and active elevated sessions.\n\n"
        "    Fields:\n"
        "      session_timeout_s  Time before elevated access expires\n"
        "      time_skew          Allowed clock drift (+/- time steps)\n"
        "      active_sessions    Number of users with elevated access\n"
        "      sessions[]         List of active session user IDs/names\n\n"
        "    Users authenticate via DTMF *68<6-digit code># using a\n"
        "    Google Authenticator compatible TOTP token (RFC 6238).\n"
        "    Grants time-limited elevated access for privileged commands.\n\n"
        "Config: [otp] session_timeout, time_skew\n"
        "User:   [user.N] totp_secret = <base32 key>\n"
        "DTMF:   *68<code>#\n");
    resp_str(r, "error", "usage: otp [help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "otp", .usage = "otp", .description = "OTP authentication status", .handler = cli_otp, .category = "Control" },
};

static kerchunk_module_def_t mod_otp = {
    .name             = "mod_otp",
    .version          = "1.0.0",
    .description      = "TOTP authentication for elevated access",
    .load             = otp_load,
    .configure        = otp_configure,
    .unload           = otp_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_otp);
