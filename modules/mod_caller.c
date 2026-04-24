/*
 * mod_caller.c — Caller identification
 *
 * Methods:
 * - DTMF ANI capture (first N ms after COR assert)
 * - DTMF login (*<code># — persists for configurable session timeout)
 *
 * DTMF login sessions survive COR drops so the user doesn't have to
 * re-login every transmission. Session expires after login_timeout_ms
 * of inactivity (no COR assert resets the timer).
 *
 * Note: CTCSS/DCS tones are NOT used for caller identification.
 * They are reserved for repeater access (squelch gating), tone-based
 * action routing (mod_route), and selective calling (group TX tones).
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOG_MOD "caller"
#define MAX_ANI_LEN 8

static kerchunk_core_t *g_core;

/* Current caller */
static int  g_current_user_id = 0;
static int  g_current_method  = 0;

/* ANI capture */
static int  g_ani_active;
static char g_ani_buf[MAX_ANI_LEN + 1];
static int  g_ani_pos;
static int  g_ani_timer = -1;
static int  g_ani_window_ms = 1000;

/* DTMF login state */
static int  g_login_active;
static char g_login_buf[MAX_ANI_LEN + 1];
static int  g_login_pos;

/* Login session */
static int    g_session_user_id    = 0;     /* persists across COR drops */
static int    g_session_timer      = -1;
static int    g_login_timeout_ms   = 1800000; /* 30 min default */
static time_t g_session_started_at = 0;     /* wall-clock; resets on refresh */

/* Optional DTMF logout code. Empty string = disabled (no DTMF logout).
 * When set and the user is logged in, dialing *<logout_code># clears
 * the active session. Configured via [caller] logout_code. */
static char g_logout_code[MAX_ANI_LEN + 1] = "";

static const char *method_name(int m)
{
    switch (m) {
    case KERCHUNK_CALLER_DTMF_ANI:   return "ANI";
    case KERCHUNK_CALLER_DTMF_LOGIN: return "LOGIN";
    default:                       return "?";
    }
}

/* Publish authoritative caller-session snapshot to admin dashboard.
 * mod_web caches the last value per type and replays to new SSE
 * subscribers, so this only needs to fire on state transitions.
 * Sensitive (names + login state) → admin_only=1. */
static void publish_session_snapshot(void)
{
    if (!g_core || !g_core->sse_publish) return;

    int authenticated = (g_session_user_id > 0);
    const kerchunk_user_t *u = authenticated
        ? g_core->user_lookup_by_id(g_session_user_id) : NULL;

    long remaining_sec = 0;
    if (authenticated && g_session_started_at > 0) {
        long elapsed = (long)(time(NULL) - g_session_started_at);
        long total   = g_login_timeout_ms / 1000;
        remaining_sec = (total > elapsed) ? (total - elapsed) : 0;
    }

    char json[512];
    snprintf(json, sizeof(json),
        "{\"authenticated\":%s,"
        "\"user_id\":%d,"
        "\"user_name\":\"%s\","
        "\"method\":\"%s\","
        "\"current_user_id\":%d,"
        "\"session_started_at\":%lld,"
        "\"session_remaining_sec\":%ld,"
        "\"session_timeout_sec\":%d}",
        authenticated ? "true" : "false",
        g_session_user_id,
        u ? u->name : "",
        authenticated ? method_name(KERCHUNK_CALLER_DTMF_LOGIN) : "",
        g_current_user_id,
        (long long)g_session_started_at,
        remaining_sec,
        g_login_timeout_ms / 1000);

    g_core->sse_publish("caller_session_updated", json, /*admin_only=*/1);
}

static void fire_identified(int user_id, int method)
{
    g_current_user_id = user_id;
    g_current_method  = method;

    kerchevt_t evt = {
        .type = KERCHEVT_CALLER_IDENTIFIED,
        .caller = { .user_id = user_id, .method = method },
    };
    g_core->fire_event(&evt);

    const kerchunk_user_t *u = g_core->user_lookup_by_id(user_id);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "identified: %s (id=%d, via %s)",
                u ? u->name : "unknown", user_id, method_name(method));

    publish_session_snapshot();
}

static void fire_cleared(const char *reason)
{
    if (g_current_user_id == 0)
        return;

    const kerchunk_user_t *u = g_core->user_lookup_by_id(g_current_user_id);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "cleared: %s (id=%d, %s)",
                u ? u->name : "unknown", g_current_user_id,
                reason ? reason : "unkeyed");

    g_current_user_id = 0;
    g_current_method  = 0;

    kerchevt_t evt = { .type = KERCHEVT_CALLER_CLEARED };
    g_core->fire_event(&evt);

    publish_session_snapshot();
}

/* ---- Login session management ---- */

static void session_timeout_cb(void *ud)
{
    (void)ud;
    g_session_timer = -1;

    if (g_session_user_id > 0) {
        const kerchunk_user_t *u = g_core->user_lookup_by_id(g_session_user_id);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                     "login session expired: %s (id=%d, after %ds)",
                     u ? u->name : "unknown", g_session_user_id,
                     g_login_timeout_ms / 1000);
        g_session_user_id    = 0;
        g_session_started_at = 0;
        /* If this user is still the current caller, clear them */
        if (g_current_user_id > 0 &&
            g_current_method == KERCHUNK_CALLER_DTMF_LOGIN)
            fire_cleared("session expired");
        publish_session_snapshot();

        /* Audible logout — only if TTS is available; tones in this
         * context could be confused with other repeater alerts. */
        if (g_core->tts_speak) {
            char msg[96];
            if (u && u->name[0])
                snprintf(msg, sizeof(msg),
                         "%s, your login session has expired.", u->name);
            else
                snprintf(msg, sizeof(msg), "Login session expired.");
            g_core->tts_speak(msg, KERCHUNK_PRI_LOW);
        }
    }
}

static void session_start(int user_id)
{
    g_session_user_id    = user_id;
    g_session_started_at = time(NULL);

    /* Reset session timer */
    if (g_session_timer >= 0)
        g_core->timer_cancel(g_session_timer);
    g_session_timer = g_core->timer_create(
        g_login_timeout_ms, 0, session_timeout_cb, NULL);

    const kerchunk_user_t *u = g_core->user_lookup_by_id(user_id);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                 "login session started: %s (id=%d, timeout=%ds)",
                 u ? u->name : "unknown", user_id,
                 g_login_timeout_ms / 1000);

    publish_session_snapshot();

    /* Audible login confirmation. Only fires here (in session_start),
     * not in fire_identified, so the welcome only plays on a fresh
     * DTMF login — not on every COR re-identify. */
    if (g_core->tts_speak) {
        char msg[96];
        if (u && u->name[0])
            snprintf(msg, sizeof(msg), "Welcome %s, you are logged in.", u->name);
        else
            snprintf(msg, sizeof(msg), "Logged in.");
        g_core->tts_speak(msg, KERCHUNK_PRI_NORMAL);
    } else {
        /* Ascending arpeggio: 600-800-1000 Hz, 80 ms each */
        g_core->queue_tone(600,  80, 4000, KERCHUNK_PRI_LOW);
        g_core->queue_tone(800,  80, 4000, KERCHUNK_PRI_LOW);
        g_core->queue_tone(1000, 80, 4000, KERCHUNK_PRI_LOW);
    }
}

static void session_refresh(void)
{
    if (g_session_user_id <= 0 || g_session_timer < 0)
        return;

    /* Reset the timeout on each COR assert (user is active) */
    g_core->timer_cancel(g_session_timer);
    g_session_timer = g_core->timer_create(
        g_login_timeout_ms, 0, session_timeout_cb, NULL);
    g_session_started_at = time(NULL);

    publish_session_snapshot();
}

static void session_clear(void)
{
    if (g_session_timer >= 0) {
        g_core->timer_cancel(g_session_timer);
        g_session_timer = -1;
    }
    g_session_user_id    = 0;
    g_session_started_at = 0;
    publish_session_snapshot();
}

/* Explicit logout — user-initiated (DTMF or CLI). Speaks goodbye and
 * fires CALLER_CLEARED so other modules can react. Idempotent: a no-op
 * if there's no active session. */
static void do_logout(const char *reason)
{
    if (g_session_user_id <= 0)
        return;

    const kerchunk_user_t *u = g_core->user_lookup_by_id(g_session_user_id);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "logout: %s (id=%d, %s)",
                u ? u->name : "unknown", g_session_user_id,
                reason ? reason : "explicit");

    if (g_core->tts_speak) {
        char msg[96];
        if (u && u->name[0])
            snprintf(msg, sizeof(msg), "Goodbye %s, you are logged out.", u->name);
        else
            snprintf(msg, sizeof(msg), "Logged out.");
        g_core->tts_speak(msg, KERCHUNK_PRI_NORMAL);
    } else {
        /* Descending arpeggio: 1000-800-600 Hz, 80 ms each */
        g_core->queue_tone(1000, 80, 4000, KERCHUNK_PRI_LOW);
        g_core->queue_tone(800,  80, 4000, KERCHUNK_PRI_LOW);
        g_core->queue_tone(600,  80, 4000, KERCHUNK_PRI_LOW);
    }

    session_clear();
    if (g_current_user_id > 0)
        fire_cleared(reason ? reason : "logout");
}

/* ---- ANI timeout ---- */

static void ani_timeout(void *ud)
{
    (void)ud;
    g_ani_timer = -1;
    g_ani_active = 0;

    if (g_ani_pos > 0) {
        g_ani_buf[g_ani_pos] = '\0';
        const kerchunk_user_t *u = g_core->user_lookup_by_ani(g_ani_buf);
        if (u)
            fire_identified(u->id, KERCHUNK_CALLER_DTMF_ANI);
        else
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "ANI '%s' not found", g_ani_buf);
    }
}

/* ---- Event handlers ---- */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;

    /* Arm ANI capture — but don't reset if already collecting digits.
     * COR can briefly drop during DTMF (RT97L drops COS when CTCSS is
     * interrupted by DTMF tones), causing a COR reassert that would
     * otherwise wipe out digits already captured. */
    if (!g_ani_active) {
        g_ani_active = 1;
        g_ani_pos    = 0;
        g_ani_buf[0] = '\0';
    }

    /* If there's an active login session, re-identify on key-up */
    if (g_session_user_id > 0) {
        session_refresh();
        if (g_current_user_id != g_session_user_id)
            fire_identified(g_session_user_id, KERCHUNK_CALLER_DTMF_LOGIN);
    }
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;

    /* Cancel ANI capture */
    if (g_ani_timer >= 0) {
        g_core->timer_cancel(g_ani_timer);
        g_ani_timer = -1;
    }
    g_ani_active = 0;

    /* Clear caller — but login sessions persist */
    if (g_current_method == KERCHUNK_CALLER_DTMF_LOGIN) {
        /* Don't clear — session survives COR drop.
         * Just clear the "active caller" so other modules know
         * nobody is currently transmitting, but keep session. */
        g_current_user_id = 0;
        g_current_method  = 0;
        kerchevt_t clr = { .type = KERCHEVT_CALLER_CLEARED };
        g_core->fire_event(&clr);
    } else {
        fire_cleared("unkeyed");
    }
}

static void on_dtmf_digit(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    char d = evt->dtmf.digit;

    /* ANI capture (during window) */
    if (g_ani_active && g_ani_pos < MAX_ANI_LEN) {
        /* * starts a login sequence — close ANI window immediately
         * and fall through to the login handler below. */
        if (d == '*') {
            g_ani_active = 0;
            if (g_ani_timer >= 0) {
                g_core->timer_cancel(g_ani_timer);
                g_ani_timer = -1;
            }
            /* process any ANI digits collected so far */
            if (g_ani_pos > 0) {
                g_ani_buf[g_ani_pos] = '\0';
                const kerchunk_user_t *u = g_core->user_lookup_by_ani(g_ani_buf);
                if (u)
                    fire_identified(u->id, KERCHUNK_CALLER_DTMF_ANI);
            }
            /* fall through to login handler */
        } else {
            /* Start timer on first digit — gives the full window for the
             * complete ANI sequence regardless of COR-to-DTMF delay */
            if (g_ani_pos == 0 && g_ani_timer < 0)
                g_ani_timer = g_core->timer_create(g_ani_window_ms, 0,
                                                   ani_timeout, NULL);
            g_ani_buf[g_ani_pos++] = d;
            return;
        }
    }

    /* DTMF *<code># sequence handling. We always capture, regardless of
     * login state, and decide at '#' time:
     *
     *   - if logged in: try matching the logout code (silent if no match,
     *     because most *<code># sequences are normal DTMF commands)
     *   - if not logged in: try matching a user's dtmf_login code
     *
     * The previous design skipped capture entirely when logged in, which
     * meant logout via DTMF was impossible. */
    if (d == '*') {
        g_login_active = 1;
        g_login_pos = 0;
        g_login_buf[0] = '\0';
        return;
    }

    if (g_login_active) {
        if (d == '#') {
            g_login_buf[g_login_pos] = '\0';
            g_login_active = 0;

            const char *code = g_login_buf;

            if (g_session_user_id > 0) {
                /* Already logged in — only act on the logout code */
                if (g_logout_code[0] && strcmp(code, g_logout_code) == 0) {
                    do_logout("dtmf logout");
                }
                /* Otherwise silently ignore — most *<code># sequences
                 * are normal DTMF commands handled by mod_dtmfcmd. */
                return;
            }

            /* Not logged in — search all users for matching login code */
            int ucount = g_core->user_count();
            for (int i = 0; i < ucount; i++) {
                const kerchunk_user_t *u = kerchunk_user_get(i);
                if (u && u->dtmf_login[0] != '\0' &&
                    strcmp(u->dtmf_login, code) == 0) {
                    fire_identified(u->id, KERCHUNK_CALLER_DTMF_LOGIN);
                    session_start(u->id);
                    return;
                }
            }
            /* Silent failure — every DTMF command (*84#, *87#, etc.)
             * also lands in this handler, and we don't want to speak
             * "Login failed" for every command that isn't also a login
             * code. Logged at DEBUG so operators can still see attempts. */
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                         "login attempt: code '%s' did not match any user",
                         code);
        } else if (g_login_pos < MAX_ANI_LEN) {
            g_login_buf[g_login_pos++] = d;
        } else {
            g_login_active = 0;
        }
    }
}

/* ---- Module lifecycle ---- */

static int caller_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_COR_ASSERT, on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,   on_cor_drop, NULL);
    core->subscribe(KERCHEVT_DTMF_DIGIT, on_dtmf_digit, NULL);
    return 0;
}

static int caller_configure(const kerchunk_config_t *cfg)
{
    g_ani_window_ms    = kerchunk_config_get_duration_ms(cfg, "caller", "ani_window", 1000);
    g_login_timeout_ms = kerchunk_config_get_duration_ms(cfg, "caller", "login_timeout", 1800000);

    const char *lc = kerchunk_config_get(cfg, "caller", "logout_code");
    if (lc && lc[0])
        snprintf(g_logout_code, sizeof(g_logout_code), "%s", lc);
    else
        g_logout_code[0] = '\0';

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "ani_window=%dms login_timeout=%ds logout_code=%s",
                g_ani_window_ms, g_login_timeout_ms / 1000,
                g_logout_code[0] ? g_logout_code : "(disabled)");

    /* Seed the SSE cache so admin dashboards connecting before any
     * login activity see an authoritative "no session" payload. */
    publish_session_snapshot();
    return 0;
}

static void caller_unload(void)
{
    g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,   on_cor_drop);
    g_core->unsubscribe(KERCHEVT_DTMF_DIGIT, on_dtmf_digit);
    if (g_ani_timer >= 0)
        g_core->timer_cancel(g_ani_timer);
    session_clear();
}

/* CLI */
static int cli_caller(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    if (argc >= 2 && strcmp(argv[1], "logout") == 0) {
        if (g_session_user_id <= 0) {
            resp_str(r, "status", "no active session");
            return 0;
        }
        const kerchunk_user_t *u = g_core->user_lookup_by_id(g_session_user_id);
        char who[64];
        snprintf(who, sizeof(who), "%s", u ? u->name : "unknown");
        do_logout("cli logout");
        resp_str(r, "status", "logged out");
        resp_str(r, "user", who);
        return 0;
    }

    if (g_current_user_id > 0) {
        const kerchunk_user_t *u = g_core->user_lookup_by_id(g_current_user_id);
        resp_str(r, "active_caller", u ? u->name : "unknown");
        resp_int(r, "caller_id", g_current_user_id);
        resp_str(r, "method", method_name(g_current_method));
    } else {
        resp_str(r, "active_caller", "none");
    }
    if (g_session_user_id > 0) {
        const kerchunk_user_t *u = g_core->user_lookup_by_id(g_session_user_id);
        resp_str(r, "login_session", u ? u->name : "unknown");
        resp_int(r, "session_id", g_session_user_id);
        resp_int(r, "session_timeout_s", g_login_timeout_ms / 1000);
    }
    return 0;

usage:
    resp_text_raw(r, "Caller identification module\n\n"
        "  caller\n"
        "    Show the currently identified caller, identification method\n"
        "    (ANI or LOGIN), and active login session if any.\n\n"
        "  caller logout\n"
        "    Clear the active DTMF login session immediately.\n"
        "    Equivalent to dialing the configured logout_code on the air.\n\n"
        "    Fields:\n"
        "      active_caller   Current caller name (or \"none\")\n"
        "      caller_id       Numeric user ID\n"
        "      method          ANI (automatic) or LOGIN (*code#)\n"
        "      login_session   Persistent login session user (survives COR drops)\n"
        "      session_timeout_s  Session expiry in seconds\n\n"
        "Config: [caller] ani_window, login_timeout, logout_code\n");
    resp_str(r, "error", "usage: caller [logout|help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "caller", .usage = "caller [logout]",
      .description = "Show or end the active caller session",
      .handler = cli_caller, .category = "Identification",
      .subcommands = "logout" },
};

static kerchunk_module_def_t mod_caller = {
    .name         = "mod_caller",
    .version      = "1.0.0",
    .description  = "Caller identification",
    .load         = caller_load,
    .configure    = caller_configure,
    .unload       = caller_unload,
    .cli_commands = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_caller);
