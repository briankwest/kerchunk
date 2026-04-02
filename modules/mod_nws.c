/*
 * mod_nws.c — National Weather Service alert monitor
 *
 * Polls api.weather.gov for active alerts at a configurable location.
 * Tracks alerts by ID, announces new/active/expired alerts via TTS.
 * Re-announces active alerts every 15 minutes.
 *
 * DTMF: *96# for on-demand alert readout.
 * Requires: libcurl, mod_tts (for announcements)
 *
 * Config: [nws] section in kerchunk.conf
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <curl/curl.h>

#define LOG_MOD "nws"

/* DTMF event — *96# */
#define DTMF_EVT_NWS (KERCHEVT_CUSTOM + 14)

/* Alert severity levels */
#define SEV_UNKNOWN  0
#define SEV_MINOR    1
#define SEV_MODERATE 2
#define SEV_SEVERE   3
#define SEV_EXTREME  4

/* Alert tracking */
#define MAX_ALERTS     32
#define ALERT_ID_LEN   128
#define ALERT_EVT_LEN  64
#define ALERT_HDL_LEN  256

typedef struct {
    int    active;
    char   id[ALERT_ID_LEN];
    char   event[ALERT_EVT_LEN];
    char   headline[ALERT_HDL_LEN];
    int    severity;
    time_t first_seen;
    time_t last_announced;
} tracked_alert_t;

static kerchunk_core_t *g_core;

/* Config */
static int    g_enabled          = 0;
static double g_lat              = 0.0;
static double g_lon              = 0.0;
static char   g_contact[128]    = "repeater@example.com";
static int    g_poll_interval_ms = 300000;   /* 5 min */
static int    g_reannounce_ms    = 900000;   /* 15 min */
static int    g_min_severity     = SEV_MODERATE;
static int    g_auto_announce    = 1;
static int    g_attention_tones  = 1;

/* State */
static tracked_alert_t g_alerts[MAX_ALERTS];
static int    g_alert_count;
static time_t g_last_poll;
static int    g_poll_timer       = -1;
static int    g_reannounce_timer = -1;

/* ── libcurl helpers (same pattern as mod_weather) ── */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} curl_buf_t;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    curl_buf_t *buf = ud;
    if (size > 0 && nmemb > SIZE_MAX / size) return 0;
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

static char *nws_fetch(void)
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.weather.gov/alerts/active?point=%.4f,%.4f",
             g_lat, g_lon);

    char ua[256];
    snprintf(ua, sizeof(ua), "kerchunkd/" KERCHUNK_VERSION_STRING " (%s)", g_contact);

    curl_buf_t buf = { .data = malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.data) return NULL;
    buf.data[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) { free(buf.data); return NULL; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/geo+json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, 10L * 1024 * 1024);  /* 10MB */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "fetch failed: %s",
                    curl_easy_strerror(res));
        free(buf.data);
        return NULL;
    }

    g_last_poll = time(NULL);
    return buf.data;
}

/* ── JSON parsing helpers ── */

/*
 * Naive JSON string extractor using strstr(). This can produce incorrect
 * results if a key name appears inside a string value earlier in the JSON.
 * A proper fix would use a real JSON parser (e.g., mg_json_get_str() from
 * mongoose). Acceptable here because process_alerts() works on isolated
 * per-"properties" blocks, limiting the scope for false matches.
 */
static int json_str(const char *json, const char *key, char *out, size_t max)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    size_t len = (size_t)(end - p);
    if (len >= max) len = max - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/* ── Severity parsing ── */

static int parse_severity(const char *s)
{
    if (!s) return SEV_UNKNOWN;
    if (strcmp(s, "Extreme") == 0)  return SEV_EXTREME;
    if (strcmp(s, "Severe") == 0)   return SEV_SEVERE;
    if (strcmp(s, "Moderate") == 0) return SEV_MODERATE;
    if (strcmp(s, "Minor") == 0)    return SEV_MINOR;
    return SEV_UNKNOWN;
}

static const char *severity_str(int sev)
{
    switch (sev) {
    case SEV_EXTREME:  return "EXTREME";
    case SEV_SEVERE:   return "SEVERE";
    case SEV_MODERATE: return "MODERATE";
    case SEV_MINOR:    return "MINOR";
    default:           return "UNKNOWN";
    }
}

static int severity_to_priority(int sev)
{
    switch (sev) {
    case SEV_EXTREME:  return 8;
    case SEV_SEVERE:   return 6;
    case SEV_MODERATE: return 4;
    case SEV_MINOR:    return 2;
    default:           return 2;
    }
}

/* ── Alert announcement ── */

static void announce_attention_tones(int priority)
{
    if (!g_attention_tones) return;
    g_core->queue_tone(950, 500, 6000, priority);
    g_core->queue_tone(1400, 500, 6000, priority);
    g_core->queue_tone(1750, 500, 6000, priority);
    g_core->queue_silence(300, priority);
}

static void announce_new_alert(tracked_alert_t *a)
{
    int pri = severity_to_priority(a->severity);

    if (a->severity >= SEV_EXTREME)
        announce_attention_tones(pri);

    if (g_core->tts_speak) {
        char text[512];
        snprintf(text, sizeof(text),
                 "Attention. National Weather Service %s. %s",
                 a->event, a->headline);
        g_core->tts_speak(text, pri);
    } else {
        /* Fallback: just queue tones if TTS not loaded */
        g_core->queue_tone(800, 500, 4000, pri);
    }

    a->last_announced = time(NULL);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "nws", .description = a->event } };
    kerchevt_fire(&ae);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "NEW: [%s] %s",
                severity_str(a->severity), a->event);
}

static void announce_reminder(tracked_alert_t *a)
{
    int pri = severity_to_priority(a->severity);

    if (g_core->tts_speak) {
        char text[256];
        snprintf(text, sizeof(text),
                 "Reminder. %s remains in effect.", a->event);
        g_core->tts_speak(text, pri);
    }

    a->last_announced = time(NULL);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "nws", .description = a->event } };
    kerchevt_fire(&ae);
}

static void announce_expired(tracked_alert_t *a)
{
    if (g_core->tts_speak) {
        char text[256];
        snprintf(text, sizeof(text), "The %s has expired.", a->event);
        g_core->tts_speak(text, KERCHUNK_PRI_NORMAL);
    }

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "nws", .description = a->event } };
    kerchevt_fire(&ae);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "EXPIRED: %s", a->event);
}

/* ── Alert tracking ── */

static tracked_alert_t *find_alert(const char *id)
{
    for (int i = 0; i < MAX_ALERTS; i++) {
        if (g_alerts[i].active && strcmp(g_alerts[i].id, id) == 0)
            return &g_alerts[i];
    }
    return NULL;
}

static tracked_alert_t *add_alert(const char *id, const char *event,
                                   const char *headline, int severity)
{
    for (int i = 0; i < MAX_ALERTS; i++) {
        if (!g_alerts[i].active) {
            tracked_alert_t *a = &g_alerts[i];
            a->active = 1;
            snprintf(a->id, sizeof(a->id), "%s", id);
            snprintf(a->event, sizeof(a->event), "%s", event);
            snprintf(a->headline, sizeof(a->headline), "%s", headline);
            a->severity = severity;
            a->first_seen = time(NULL);
            a->last_announced = 0;
            g_alert_count++;
            return a;
        }
    }
    g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "max alerts (%d) reached — dropping: %s", MAX_ALERTS, event);
    return NULL;
}

static void remove_alert(tracked_alert_t *a)
{
    a->active = 0;
    g_alert_count--;
}

/* ── Poll + process ── */

static void process_alerts(const char *json)
{
    /* Mark all as "not seen" */
    int seen[MAX_ALERTS] = {0};

    /* Walk through each "properties" block */
    const char *p = json;
    while ((p = strstr(p, "\"properties\"")) != NULL) {
        p += 12;  /* skip "properties" */

        /* Find the block boundary — look for next "properties" or end */
        const char *block_end = strstr(p + 1, "\"properties\"");
        if (!block_end) block_end = p + strlen(p);

        /* Work on a copy of this block so json_str doesn't jump to next block */
        size_t blen = (size_t)(block_end - p);
        char *block = malloc(blen + 1);
        if (!block) break;
        memcpy(block, p, blen);
        block[blen] = '\0';

        char id[ALERT_ID_LEN] = "";
        char event[ALERT_EVT_LEN] = "";
        char severity[32] = "";
        char headline[ALERT_HDL_LEN] = "";
        char status[16] = "";

        json_str(block, "id", id, sizeof(id));
        json_str(block, "event", event, sizeof(event));
        json_str(block, "severity", severity, sizeof(severity));
        json_str(block, "headline", headline, sizeof(headline));
        json_str(block, "status", status, sizeof(status));

        free(block);

        /* Skip non-actual, empty, or below-threshold */
        if (strcmp(status, "Actual") != 0 || id[0] == '\0')
            goto next;

        int sev = parse_severity(severity);
        if (sev < g_min_severity)
            goto next;

        /* Check if we already track this alert */
        tracked_alert_t *existing = find_alert(id);
        if (existing) {
            /* Still active — mark as seen */
            for (int i = 0; i < MAX_ALERTS; i++) {
                if (&g_alerts[i] == existing) { seen[i] = 1; break; }
            }
        } else {
            /* New alert */
            tracked_alert_t *a = add_alert(id, event, headline, sev);
            if (a) {
                for (int i = 0; i < MAX_ALERTS; i++) {
                    if (&g_alerts[i] == a) { seen[i] = 1; break; }
                }
                if (g_auto_announce)
                    announce_new_alert(a);
            }
        }

    next:
        p = block_end;
        if (p == json + strlen(json)) break;
    }

    /* Any tracked alert not seen has expired */
    for (int i = 0; i < MAX_ALERTS; i++) {
        if (g_alerts[i].active && !seen[i]) {
            announce_expired(&g_alerts[i]);
            remove_alert(&g_alerts[i]);
        }
    }
}

/* ── Background poll thread (avoids blocking main loop on curl) ── */

static int             g_poll_tid = -1;
static pthread_mutex_t g_poll_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_poll_cond  = PTHREAD_COND_INITIALIZER;
static volatile int    g_poll_requested;

static void poll_nws(void)
{
    char *json = nws_fetch();
    if (!json) return;

    process_alerts(json);
    free(json);

    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                "polled: %d active alert%s", g_alert_count,
                g_alert_count == 1 ? "" : "s");
}

static void *poll_worker(void *arg)
{
    (void)arg;
    while (!g_core->thread_should_stop(g_poll_tid)) {
        pthread_mutex_lock(&g_poll_mutex);
        /* Wait for poll request or stop signal */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;  /* wake every 1s to check stop flag */
        while (!g_poll_requested && !g_core->thread_should_stop(g_poll_tid))
            pthread_cond_timedwait(&g_poll_cond, &g_poll_mutex, &ts);
        g_poll_requested = 0;
        pthread_mutex_unlock(&g_poll_mutex);

        if (g_core->thread_should_stop(g_poll_tid)) break;
        poll_nws();
    }
    return NULL;
}

static void request_poll(void)
{
    pthread_mutex_lock(&g_poll_mutex);
    g_poll_requested = 1;
    pthread_cond_signal(&g_poll_cond);
    pthread_mutex_unlock(&g_poll_mutex);
}

/* ── Timer callbacks ── */

static void poll_timer_cb(void *ud)
{
    (void)ud;
    request_poll();  /* Signal background thread — non-blocking */
}

static void reannounce_timer_cb(void *ud)
{
    (void)ud;
    time_t now = time(NULL);
    int reannounce_secs = g_reannounce_ms / 1000;

    for (int i = 0; i < MAX_ALERTS; i++) {
        if (!g_alerts[i].active) continue;
        if (difftime(now, g_alerts[i].last_announced) >= reannounce_secs)
            announce_reminder(&g_alerts[i]);
    }
}

/* ── DTMF handler: *96# ── */

static void on_nws_dtmf(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) return;

    if (g_alert_count == 0) {
        if (g_core->tts_speak)
            g_core->tts_speak("No active weather alerts.", KERCHUNK_PRI_HIGH);
        else
            g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_HIGH);

        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "nws", .description = "no active alerts" } };
        kerchevt_fire(&ae);
        return;
    }

    /* Read out all active alerts */
    for (int i = 0; i < MAX_ALERTS; i++) {
        if (!g_alerts[i].active) continue;

        if (g_alerts[i].severity >= SEV_EXTREME)
            announce_attention_tones(severity_to_priority(g_alerts[i].severity));

        if (g_core->tts_speak) {
            char text[512];
            snprintf(text, sizeof(text), "%s alert. %s",
                     severity_str(g_alerts[i].severity),
                     g_alerts[i].headline);
            g_core->tts_speak(text, severity_to_priority(g_alerts[i].severity));
        }

        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "nws", .description = g_alerts[i].event } };
        kerchevt_fire(&ae);
    }
}

/* ── Module lifecycle ── */

static int nws_load(kerchunk_core_t *core)
{
    g_core = core;

    if (core->dtmf_register)
        core->dtmf_register("96", 14, "NWS alerts", "nws_alerts");

    core->subscribe(DTMF_EVT_NWS, on_nws_dtmf, NULL);
    return 0;
}

static int nws_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "nws", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "nws", "latitude");
    if (v) g_lat = atof(v);

    v = kerchunk_config_get(cfg, "nws", "longitude");
    if (v) g_lon = atof(v);

    v = kerchunk_config_get(cfg, "nws", "contact");
    if (v) snprintf(g_contact, sizeof(g_contact), "%s", v);

    g_poll_interval_ms = kerchunk_config_get_int(cfg, "nws", "poll_interval", 300000);
    g_reannounce_ms    = kerchunk_config_get_int(cfg, "nws", "reannounce_interval", 900000);

    v = kerchunk_config_get(cfg, "nws", "min_severity");
    if (v) {
        if (strcmp(v, "extreme") == 0)  g_min_severity = SEV_EXTREME;
        else if (strcmp(v, "severe") == 0)   g_min_severity = SEV_SEVERE;
        else if (strcmp(v, "moderate") == 0) g_min_severity = SEV_MODERATE;
        else if (strcmp(v, "minor") == 0)    g_min_severity = SEV_MINOR;
    }

    v = kerchunk_config_get(cfg, "nws", "auto_announce");
    if (v) g_auto_announce = (strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "nws", "attention_tones");
    if (v) g_attention_tones = (strcmp(v, "off") != 0);

    if (!g_enabled) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled");
        return 0;
    }

    if (g_lat == 0.0 && g_lon == 0.0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "latitude/longitude not configured");
        g_enabled = 0;
        return 0;
    }

    /* Stop existing poll thread on reload */
    if (g_poll_tid >= 0) {
        g_core->thread_stop(g_poll_tid);
        pthread_cond_signal(&g_poll_cond);
        g_core->thread_join(g_poll_tid);
        g_poll_tid = -1;
    }

    /* Start timers */
    if (g_poll_timer >= 0) g_core->timer_cancel(g_poll_timer);
    if (g_reannounce_timer >= 0) g_core->timer_cancel(g_reannounce_timer);

    g_poll_timer = g_core->timer_create(g_poll_interval_ms, 1,
                                         poll_timer_cb, NULL);
    g_reannounce_timer = g_core->timer_create(60000, 1,
                                               reannounce_timer_cb, NULL);

    /* Start background poll thread */
    g_poll_requested = 0;
    g_poll_tid = g_core->thread_create("nws-poll", poll_worker, NULL);
    if (g_poll_tid < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "failed to create poll thread");
        g_enabled = 0;
        return 0;
    }

    /* Initial poll */
    request_poll();

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "monitoring %.4f,%.4f poll=%ds reannounce=%ds min_sev=%s",
                g_lat, g_lon,
                g_poll_interval_ms / 1000, g_reannounce_ms / 1000,
                severity_str(g_min_severity));
    return 0;
}

static void nws_unload(void)
{
    /* Stop poll thread */
    if (g_poll_tid >= 0) {
        g_core->thread_stop(g_poll_tid);
        pthread_cond_signal(&g_poll_cond);
        g_core->thread_join(g_poll_tid);
        g_poll_tid = -1;
    }

    if (g_poll_timer >= 0) {
        g_core->timer_cancel(g_poll_timer);
        g_poll_timer = -1;
    }
    if (g_reannounce_timer >= 0) {
        g_core->timer_cancel(g_reannounce_timer);
        g_reannounce_timer = -1;
    }

    if (g_core->dtmf_unregister)
        g_core->dtmf_unregister("96");
    g_core->unsubscribe(DTMF_EVT_NWS, on_nws_dtmf);
    memset(g_alerts, 0, sizeof(g_alerts));
    g_alert_count = 0;
}

/* ── JSON string escaping ── */

static void nws_json_escape(const char *in, char *out, size_t max)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j < max - 2; i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out[j++] = '\\'; out[j++] = c; }
        else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (c == '\r') { out[j++] = '\\'; out[j++] = 'r'; }
        else if (c == '\t') { out[j++] = '\\'; out[j++] = 't'; }
        else if ((unsigned char)c < 0x20) { /* skip control chars */ }
        else out[j++] = c;
    }
    out[j] = '\0';
}

/* ── CLI ── */

static int cli_nws(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        if (!g_enabled) {
            resp_str(r, "status", "disabled");
            return 0;
        }
        request_poll();
        resp_bool(r, "ok", 1);
        resp_int(r, "alert_count", g_alert_count);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "announce") == 0) {
        if (!g_enabled) {
            resp_str(r, "status", "disabled");
            return 0;
        }
        /* Trigger on-demand readout (same as DTMF *96#) */
        kerchevt_t evt = { .type = DTMF_EVT_NWS };
        on_nws_dtmf(&evt, NULL);
        resp_bool(r, "ok", 1);
        resp_int(r, "alert_count", g_alert_count);
        return 0;
    }

    resp_bool(r, "enabled", g_enabled);
    resp_int64(r, "last_poll", (int64_t)g_last_poll);
    resp_int(r, "alert_count", g_alert_count);

    /* JSON: alerts array */
    if (!r->jfirst) resp_json_raw(r, ",");
    resp_json_raw(r, "\"alerts\":[");
    int jfirst = 1;
    for (int i = 0; i < MAX_ALERTS; i++) {
        if (!g_alerts[i].active) continue;
        if (!jfirst) resp_json_raw(r, ",");
        char e_id[ALERT_ID_LEN * 2], e_event[ALERT_EVT_LEN * 2];
        char e_headline[ALERT_HDL_LEN * 2];
        nws_json_escape(g_alerts[i].id, e_id, sizeof(e_id));
        nws_json_escape(g_alerts[i].event, e_event, sizeof(e_event));
        nws_json_escape(g_alerts[i].headline, e_headline, sizeof(e_headline));
        char frag[1024];
        snprintf(frag, sizeof(frag),
                 "{\"id\":\"%s\",\"event\":\"%s\",\"severity\":\"%s\","
                 "\"headline\":\"%s\"}",
                 e_id, e_event,
                 severity_str(g_alerts[i].severity),
                 e_headline);
        resp_json_raw(r, frag);
        jfirst = 0;
    }
    resp_json_raw(r, "]");
    r->jfirst = 0;

    /* Text */
    if (g_enabled) {
        char line[128];
        snprintf(line, sizeof(line), "Location: %.4f, %.4f\n", g_lat, g_lon);
        resp_text_raw(r, line);

        if (g_last_poll > 0) {
            time_t now = time(NULL);
            int ago = (int)difftime(now, g_last_poll);
            snprintf(line, sizeof(line), "Last poll: %d seconds ago\n", ago);
            resp_text_raw(r, line);
        } else {
            resp_text_raw(r, "Last poll: never\n");
        }

        for (int i = 0; i < MAX_ALERTS; i++) {
            if (!g_alerts[i].active) continue;
            snprintf(line, sizeof(line), "  [%s] %s\n",
                     severity_str(g_alerts[i].severity),
                     g_alerts[i].event);
            resp_text_raw(r, line);
        }
    }
    return 0;

usage:
    resp_text_raw(r, "National Weather Service alert monitor\n\n"
        "  nws\n"
        "    Show NWS status: enabled, last poll time, active alerts with\n"
        "    severity, event type, and headline.\n\n"
        "  nws check\n"
        "    Force an immediate poll of api.weather.gov for new alerts.\n\n"
        "  nws announce\n"
        "    Read out all active alerts via TTS (same as DTMF *96#).\n\n"
        "    Polls api.weather.gov for active alerts at a configured\n"
        "    lat/lon. Tracks alerts by ID, announces new/expired via TTS.\n"
        "    Re-announces active alerts on a configurable interval.\n"
        "    Attention tones play for extreme severity alerts.\n\n"
        "Config: [nws] enabled, latitude, longitude, contact,\n"
        "        poll_interval, reannounce_interval, min_severity,\n"
        "        auto_announce, attention_tones\n"
        "DTMF:   *96# on-demand alert readout\n");
    resp_str(r, "error", "usage: nws [check|announce]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "nws", .usage = "nws [check|announce]", .description = "NWS alert status, poll, or announce",
      .handler = cli_nws,
      .category = "Announcements", .ui_label = "NWS Alerts", .ui_type = CLI_UI_BUTTON,
      .ui_command = "nws announce" },
};

static kerchunk_module_def_t mod_nws = {
    .name             = "mod_nws",
    .version          = "1.0.0",
    .description      = "National Weather Service alert monitor",
    .load             = nws_load,
    .configure        = nws_configure,
    .unload           = nws_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_nws);
