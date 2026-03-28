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
#include <time.h>
#include <curl/curl.h>

#define LOG_MOD "cwid"
#define RATE    8000

static kerchunk_core_t *g_core;
static int g_timer_id     = -1;
static int g_solar_timer  = -1;
static int g_pending      = 0;
static int g_cwid_interval_ms = 600000;
static int g_cwid_wpm     = 20;
static int g_cwid_freq    = 800;
static int16_t g_cwid_amp = 4000;
static char g_callsign[16]  = "";
static char g_frequency[16] = "";
static char g_pl_tone[32]   = "";
static int  g_voice_id      = 1;  /* Announce frequency/PL via TTS after CW */
static int  g_quiet_start   = -1; /* hour 0-23, -1 = disabled */
static int  g_quiet_end     = -1;
static int  g_quiet_solar   = 0;  /* Use sunrise-sunset.org for quiet hours */
static int  g_solar_offset  = 0;  /* minutes: quiet starts offset after sunset, ends offset before sunrise */
static char g_latitude[16]  = "";
static char g_longitude[16] = "";

/* ── libcurl helpers ── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} curl_buf_t;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    curl_buf_t *buf = ud;
    size_t total = size * nmemb;
    if (buf->len + total + 1 > buf->cap) {
        size_t new_cap = buf->cap * 2;
        if (new_cap < buf->len + total + 1)
            new_cap = buf->len + total + 1;
        char *p = realloc(buf->data, new_cap);
        if (!p) return 0;
        buf->data = p;
        buf->cap  = new_cap;
    }
    memcpy(buf->data + buf->len, ptr, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

/* Parse ISO 8601 UTC time → local minutes-since-midnight.
 * Input: "2026-03-23T11:52:00+00:00" */
static int utc_iso_to_local_minutes(const char *iso)
{
    int year, month, day, hour, min, sec;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &year, &month, &day,
               &hour, &min, &sec) != 6)
        return -1;
    struct tm utc = {
        .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
        .tm_hour = hour, .tm_min = min, .tm_sec = sec, .tm_isdst = 0
    };
    time_t t = timegm(&utc);
    if (t == (time_t)-1) return -1;
    struct tm *local = localtime(&t);
    if (!local) return -1;
    return local->tm_hour * 60 + local->tm_min;
}

/* Find a JSON string value by key.  Writes value into out, returns 1 on success. */
static int json_find_string(const char *json, const char *key, char *out, size_t outsz)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p + strlen(needle), '"');
    if (!p) return 0;
    p++;  /* skip opening quote */
    const char *end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= outsz) return 0;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return 1;
}

static void fetch_solar_times(void *ud)
{
    (void)ud;
    if (!g_latitude[0] || !g_longitude[0]) return;

    char url[256];
    snprintf(url, sizeof(url),
             "https://api.sunrise-sunset.org/json?lat=%s&lng=%s&formatted=0",
             g_latitude, g_longitude);

    curl_buf_t buf = { .data = malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.data) return;

    CURL *curl = curl_easy_init();
    if (!curl) { free(buf.data); return; }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "solar fetch failed: %s",
                    curl_easy_strerror(res));
        free(buf.data);
        return;
    }

    /* Parse sunrise and sunset from JSON response */
    char sunrise_str[64], sunset_str[64];
    if (!json_find_string(buf.data, "sunrise", sunrise_str, sizeof(sunrise_str)) ||
        !json_find_string(buf.data, "sunset", sunset_str, sizeof(sunset_str))) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "solar parse failed");
        free(buf.data);
        return;
    }
    free(buf.data);

    int sunrise_min = utc_iso_to_local_minutes(sunrise_str);
    int sunset_min  = utc_iso_to_local_minutes(sunset_str);
    if (sunrise_min < 0 || sunset_min < 0) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "solar time conversion failed");
        return;
    }

    /* Apply offset: quiet starts offset minutes after sunset,
     * ends offset minutes before sunrise */
    int quiet_start_min = sunset_min + g_solar_offset;
    int quiet_end_min   = sunrise_min - g_solar_offset;
    if (quiet_end_min < 0) quiet_end_min += 1440;
    if (quiet_start_min >= 1440) quiet_start_min -= 1440;

    g_quiet_start = quiet_start_min / 60;
    g_quiet_end   = (quiet_end_min + 59) / 60;  /* round up */
    if (g_quiet_end >= 24) g_quiet_end -= 24;

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "solar quiet hours: %02d:00-%02d:00 (sunset %d:%02d, sunrise %d:%02d, offset %+dmin)",
                g_quiet_start, g_quiet_end,
                sunset_min / 60, sunset_min % 60,
                sunrise_min / 60, sunrise_min % 60,
                g_solar_offset);
}

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
        g_core->queue_silence(200, KERCHUNK_PRI_IDENT);
        g_core->queue_audio_buffer(buf, pos, KERCHUNK_PRI_IDENT);
        g_core->queue_silence(100, KERCHUNK_PRI_IDENT);
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

        char freq_spoken[128];
        spell_number(g_frequency, freq_spoken, sizeof(freq_spoken));

        char text[512];
        if (g_pl_tone[0])
            snprintf(text, sizeof(text), "%s repeater, %s megahertz, %s.",
                     spaced, freq_spoken, g_pl_tone);
        else
            snprintf(text, sizeof(text), "%s repeater, %s megahertz.",
                     spaced, freq_spoken);

        g_core->queue_silence(500, KERCHUNK_PRI_IDENT);  /* gap between CW and voice */
        g_core->tts_speak(text, KERCHUNK_PRI_IDENT);
    }

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "cwid", .description = g_callsign } };
    kerchevt_fire(&ae);

    g_pending = 0;
}

static int is_quiet_hour(void)
{
    if (g_quiet_start < 0 || g_quiet_end < 0) return 0;
    time_t now = time(NULL);
    int hour = localtime(&now)->tm_hour;
    if (g_quiet_start <= g_quiet_end)
        return hour >= g_quiet_start && hour < g_quiet_end;
    else  /* wraps midnight: e.g., 22-06 */
        return hour >= g_quiet_start || hour < g_quiet_end;
}

/* Milliseconds until the next clock-aligned interval boundary.
 * E.g., with a 10-min interval at 14:37, returns 3 min (→ 14:40). */
static int ms_until_next_boundary(int interval_ms)
{
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    int sec_into_hour = tm->tm_min * 60 + tm->tm_sec;
    int interval_sec = interval_ms / 1000;
    if (interval_sec <= 0) interval_sec = 600;
    int remainder = sec_into_hour % interval_sec;
    if (remainder == 0) return interval_ms;  /* on boundary — next full interval */
    return (interval_sec - remainder) * 1000;
}

static void cwid_timer_cb(void *ud)
{
    (void)ud;
    if (is_quiet_hour()) {
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "CW ID skipped (quiet hours)");
        return;
    }
    if (!g_core->is_receiving() && !g_core->is_transmitting()) {
        send_cwid();
    } else {
        g_pending = 1;
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "CW ID deferred (channel busy)");
    }
}

/* One-shot callback to align first CW ID to clock, then start repeating timer */
static void cwid_align_cb(void *ud)
{
    (void)ud;
    cwid_timer_cb(NULL);
    g_timer_id = g_core->timer_create(g_cwid_interval_ms, 1, cwid_timer_cb, NULL);
}

static void on_tail_start(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_pending && !is_quiet_hour())
        send_cwid();
}

static void on_state_change(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (evt->state.new_state == 0 && g_pending && !is_quiet_hour()) {  /* 0 = RPT_IDLE */
        /* Don't assert PTT here — the queue handles its own PTT cycle.
         * An extra ref here would never be released, leaking PTT forever. */
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

    /* Build PL/DCS tone string for voice ID.
     * Check repeater defaults first, then fall back to group.1 */
    g_pl_tone[0] = '\0';
    int tx_ctcss = kerchunk_config_get_int(cfg, "repeater", "tx_ctcss", 0);
    int tx_dcs   = kerchunk_config_get_int(cfg, "repeater", "tx_dcs", 0);
    if (tx_ctcss == 0 && tx_dcs == 0) {
        tx_ctcss = kerchunk_config_get_int(cfg, "group.1", "tx_ctcss", 0);
        tx_dcs   = kerchunk_config_get_int(cfg, "group.1", "tx_dcs", 0);
    }
    if (tx_ctcss > 0)
        snprintf(g_pl_tone, sizeof(g_pl_tone), "CTCSS %d.%d hertz",
                 tx_ctcss / 10, tx_ctcss % 10);
    else if (tx_dcs > 0)
        snprintf(g_pl_tone, sizeof(g_pl_tone), "DCS code %03d", tx_dcs);

    const char *vi = kerchunk_config_get(cfg, "repeater", "voice_id");
    if (vi) g_voice_id = (strcmp(vi, "off") != 0);

    g_quiet_start = kerchunk_config_get_int(cfg, "repeater", "quiet_start", -1);
    g_quiet_end   = kerchunk_config_get_int(cfg, "repeater", "quiet_end", -1);

    const char *qm = kerchunk_config_get(cfg, "repeater", "quiet_mode");
    g_quiet_solar = (qm && strcmp(qm, "solar") == 0);
    g_solar_offset = kerchunk_config_get_int(cfg, "repeater", "solar_offset", 0);

    const char *lat = kerchunk_config_get(cfg, "general", "latitude");
    const char *lon = kerchunk_config_get(cfg, "general", "longitude");
    if (lat) snprintf(g_latitude, sizeof(g_latitude), "%s", lat);
    if (lon) snprintf(g_longitude, sizeof(g_longitude), "%s", lon);

    /* Solar quiet hours: fetch now and refresh daily */
    if (g_solar_timer >= 0)
        g_core->timer_cancel(g_solar_timer);
    g_solar_timer = -1;
    if (g_quiet_solar && g_latitude[0] && g_longitude[0]) {
        fetch_solar_times(NULL);
        g_solar_timer = g_core->timer_create(86400000, 1, fetch_solar_times, NULL); /* 24h */
    }

    if (g_timer_id >= 0)
        g_core->timer_cancel(g_timer_id);
    int offset = ms_until_next_boundary(g_cwid_interval_ms);
    g_timer_id = g_core->timer_create(offset, 0, cwid_align_cb, NULL);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "callsign=%s interval=%dms wpm=%d freq=%d",
                g_callsign, g_cwid_interval_ms, g_cwid_wpm, g_cwid_freq);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "first CW ID in %d.%ds (clock-aligned)",
                offset / 1000, (offset % 1000) / 100);
    if (g_quiet_start >= 0 && g_quiet_end >= 0)
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "quiet hours: %02d:00-%02d:00",
                    g_quiet_start, g_quiet_end);
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
    if (g_solar_timer >= 0) {
        g_core->timer_cancel(g_solar_timer);
        g_solar_timer = -1;
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
        resp_int(r, "quiet_start", g_quiet_start);
        resp_int(r, "quiet_end", g_quiet_end);
        resp_bool(r, "quiet_now", is_quiet_hour());
        resp_str(r, "quiet_mode", g_quiet_solar ? "solar" : "fixed");
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
