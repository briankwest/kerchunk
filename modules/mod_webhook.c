/*
 * mod_webhook.c — Webhook notification module
 *
 * Fires HTTP POST requests to a configured URL when specific repeater
 * events occur.  Uses a background worker thread with a circular buffer
 * queue so event handlers never block on network I/O.
 *
 * Config: [webhook] section in kerchunk.conf
 *
 * Requires: libcurl
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <curl/curl.h>

#define LOG_MOD "webhook"

/* ── Queue constants ── */

#define QUEUE_SIZE     32
#define PAYLOAD_MAX    1024

/* ── Event name-to-type mapping ── */

typedef struct {
    const char       *name;
    kerchevt_type_t   type;
} evt_map_t;

static const evt_map_t g_event_map[] = {
    { "cor_assert",       KERCHEVT_COR_ASSERT       },
    { "cor_drop",         KERCHEVT_COR_DROP          },
    { "ptt_assert",       KERCHEVT_PTT_ASSERT        },
    { "ptt_drop",         KERCHEVT_PTT_DROP          },
    { "caller_identified", KERCHEVT_CALLER_IDENTIFIED },
    { "caller_cleared",   KERCHEVT_CALLER_CLEARED    },
    { "announcement",     KERCHEVT_ANNOUNCEMENT      },
    { "recording_saved",  KERCHEVT_RECORDING_SAVED   },
    { "rx_state_change",  KERCHEVT_RX_STATE_CHANGE   },
    { "tx_state_change",  KERCHEVT_TX_STATE_CHANGE   },
    { "rx_timeout",       KERCHEVT_RX_TIMEOUT        },
    { "shutdown",         KERCHEVT_SHUTDOWN           },
    { "config_reload",    KERCHEVT_CONFIG_RELOAD      },
};

#define EVENT_MAP_COUNT ((int)(sizeof(g_event_map) / sizeof(g_event_map[0])))

/* ── Static globals ── */

static kerchunk_core_t *g_core;

/* Config */
static int   g_enabled;
static char  g_url[512];
static char  g_secret[128];
static int   g_timeout_ms    = 5000;
static int   g_retry_count   = 2;

/* Subscribed event types — tracks which events we subscribed to so we
 * can cleanly unsubscribe on unload or config reload. */
#define MAX_SUBS 16
static kerchevt_type_t g_subs[MAX_SUBS];
static int             g_sub_count;

/* ── Circular buffer queue ── */

static char             g_queue[QUEUE_SIZE][PAYLOAD_MAX];
static int              g_queue_head;     /* next slot to write */
static int              g_queue_tail;     /* next slot to read  */
static int              g_queue_count;    /* items in queue     */
static pthread_mutex_t  g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   g_queue_cond  = PTHREAD_COND_INITIALIZER;

/* ── Worker thread ── */

static int              g_worker_tid = -1;

/* ── Stats ── */

static uint64_t         g_total_sent;
static uint64_t         g_total_failed;

/* ── curl write callback — discard response body ── */

static size_t discard_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    (void)ptr; (void)ud;
    return size * nmemb;
}

/* ── Enqueue a JSON payload (non-blocking) ── */

static void enqueue_payload(const char *json)
{
    pthread_mutex_lock(&g_queue_mutex);

    if (g_queue_count >= QUEUE_SIZE) {
        /* Drop oldest entry to make room */
        g_queue_tail = (g_queue_tail + 1) % QUEUE_SIZE;
        g_queue_count--;
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "queue full — dropped oldest webhook payload");
    }

    snprintf(g_queue[g_queue_head], PAYLOAD_MAX, "%s", json);
    g_queue_head = (g_queue_head + 1) % QUEUE_SIZE;
    g_queue_count++;

    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);
}

/* ── POST a single payload with retry ── */

static int post_payload(const char *json)
{
    CURL *curl = curl_easy_init();
    if (!curl) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "curl_easy_init failed");
        return -1;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (g_secret[0]) {
        char hdr[256];
        snprintf(hdr, sizeof(hdr), "X-Webhook-Secret: %s", g_secret);
        headers = curl_slist_append(headers, hdr);
    }

    char ua[128];
    snprintf(ua, sizeof(ua), "kerchunkd/" KERCHUNK_VERSION_STRING " mod_webhook");

    curl_easy_setopt(curl, CURLOPT_URL, g_url);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, 1L * 1024 * 1024);  /* 1MB for webhook responses */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)g_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    int success = 0;
    int attempts = 1 + g_retry_count;  /* initial try + retries */

    for (int i = 0; i < attempts && !success; i++) {
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code >= 200 && http_code < 300) {
                success = 1;
            } else {
                g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                            "POST returned HTTP %ld (attempt %d/%d)",
                            http_code, i + 1, attempts);
            }
        } else {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "POST failed: %s (attempt %d/%d)",
                        curl_easy_strerror(res), i + 1, attempts);
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return success ? 0 : -1;
}

/* ── Worker thread ── */

static void *webhook_worker(void *arg)
{
    (void)arg;

    while (!g_core->thread_should_stop(g_worker_tid)) {
        char payload[PAYLOAD_MAX];

        pthread_mutex_lock(&g_queue_mutex);
        while (g_queue_count == 0 && !g_core->thread_should_stop(g_worker_tid)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_queue_cond, &g_queue_mutex, &ts);
        }

        if (g_core->thread_should_stop(g_worker_tid)) {
            pthread_mutex_unlock(&g_queue_mutex);
            break;
        }

        /* Dequeue one payload */
        memcpy(payload, g_queue[g_queue_tail], PAYLOAD_MAX);
        g_queue_tail = (g_queue_tail + 1) % QUEUE_SIZE;
        g_queue_count--;
        pthread_mutex_unlock(&g_queue_mutex);

        /* POST it (may block on network) */
        if (post_payload(payload) == 0) {
            g_total_sent++;
            g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "sent webhook");
        } else {
            g_total_failed++;
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "webhook delivery failed (total_failed=%llu)",
                        (unsigned long long)g_total_failed);
        }
    }

    /* Drain remaining items on shutdown */
    pthread_mutex_lock(&g_queue_mutex);
    while (g_queue_count > 0) {
        char payload[PAYLOAD_MAX];
        memcpy(payload, g_queue[g_queue_tail], PAYLOAD_MAX);
        g_queue_tail = (g_queue_tail + 1) % QUEUE_SIZE;
        g_queue_count--;
        pthread_mutex_unlock(&g_queue_mutex);

        if (post_payload(payload) == 0)
            g_total_sent++;
        else
            g_total_failed++;

        pthread_mutex_lock(&g_queue_mutex);
    }
    pthread_mutex_unlock(&g_queue_mutex);

    return NULL;
}

/* ── Event handler — converts event to JSON and enqueues ── */

static void on_event(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_enabled || g_worker_tid < 0) return;

    char json[PAYLOAD_MAX];
    int jlen = kerchevt_to_json(evt, json, sizeof(json));
    if (jlen <= 0 || jlen >= (int)sizeof(json))
        return;

    enqueue_payload(json);
}

/* ── Subscription management ── */

static void unsubscribe_all(void)
{
    for (int i = 0; i < g_sub_count; i++)
        g_core->unsubscribe(g_subs[i], on_event);
    g_sub_count = 0;
}

static void subscribe_events(const char *events_str)
{
    if (!events_str || events_str[0] == '\0')
        return;

    /* Work on a mutable copy — strtok modifies in place */
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", events_str);

    char *saveptr = NULL;
    char *tok = strtok_r(buf, ",", &saveptr);

    while (tok && g_sub_count < MAX_SUBS) {
        /* Strip leading/trailing whitespace */
        while (*tok == ' ') tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && *end == ' ') *end-- = '\0';

        if (*tok == '\0') {
            tok = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        int found = 0;
        for (int i = 0; i < EVENT_MAP_COUNT; i++) {
            if (strcmp(tok, g_event_map[i].name) == 0) {
                g_core->subscribe(g_event_map[i].type, on_event, NULL);
                g_subs[g_sub_count++] = g_event_map[i].type;
                g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                            "subscribed to %s", tok);
                found = 1;
                break;
            }
        }

        if (!found)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "unknown event name '%s' — skipped", tok);

        tok = strtok_r(NULL, ",", &saveptr);
    }
}

/* ── Stop worker thread safely ── */

static void stop_worker(void)
{
    if (g_worker_tid < 0) return;

    g_core->thread_stop(g_worker_tid);
    pthread_cond_signal(&g_queue_cond);
    g_core->thread_join(g_worker_tid);
    g_worker_tid = -1;
}

/* ── Module lifecycle ── */

static int webhook_load(kerchunk_core_t *core)
{
    g_core = core;
    return 0;
}

static int webhook_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    /* Stop existing worker and unsubscribe before reconfiguring */
    stop_worker();
    unsubscribe_all();

    /* Reset queue state */
    pthread_mutex_lock(&g_queue_mutex);
    g_queue_head  = 0;
    g_queue_tail  = 0;
    g_queue_count = 0;
    pthread_mutex_unlock(&g_queue_mutex);

    /* Read config */
    v = kerchunk_config_get(cfg, "webhook", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "webhook", "url");
    if (v) snprintf(g_url, sizeof(g_url), "%s", v);
    else   g_url[0] = '\0';

    v = kerchunk_config_get(cfg, "webhook", "secret");
    if (v) snprintf(g_secret, sizeof(g_secret), "%s", v);
    else   g_secret[0] = '\0';

    g_timeout_ms  = kerchunk_config_get_duration_ms(cfg, "webhook", "timeout_ms", 5000);
    g_retry_count = kerchunk_config_get_int(cfg, "webhook", "retry_count", 2);

    if (!g_enabled) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled");
        return 0;
    }

    if (g_url[0] == '\0') {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "enabled but no url configured — disabling");
        g_enabled = 0;
        return 0;
    }

    /* Subscribe to configured events */
    v = kerchunk_config_get(cfg, "webhook", "events");
    if (v) {
        subscribe_events(v);
    } else {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "no events configured — nothing to send");
    }

    /* Start worker thread */
    g_worker_tid = g_core->thread_create("webhook-send", webhook_worker, NULL);
    if (g_worker_tid < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "failed to create worker thread");
        g_enabled = 0;
        unsubscribe_all();
        return 0;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "started url=%s events=%d secret=%s timeout=%dms retry=%d",
                g_url, g_sub_count,
                g_secret[0] ? "yes" : "no",
                g_timeout_ms, g_retry_count);
    return 0;
}

static void webhook_unload(void)
{
    g_enabled = 0;
    stop_worker();
    unsubscribe_all();
}

/* ── CLI ── */

static int cli_webhook(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    /* webhook test */
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        if (!g_enabled || g_worker_tid < 0) {
            resp_str(r, "error", "webhook is not enabled");
            return 0;
        }

        enqueue_payload("{\"type\":\"test\",\"message\":\"webhook test\"}");
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "test payload enqueued");
        return 0;
    }

    /* webhook (status) */
    resp_bool(r, "enabled", g_enabled);
    resp_str(r, "url", g_url);
    resp_bool(r, "secret_configured", g_secret[0] ? 1 : 0);
    resp_int(r, "timeout_ms", g_timeout_ms);
    resp_int(r, "retry_count", g_retry_count);
    resp_int(r, "subscribed_events", g_sub_count);

    pthread_mutex_lock(&g_queue_mutex);
    int pending = g_queue_count;
    pthread_mutex_unlock(&g_queue_mutex);

    resp_int(r, "pending", pending);
    resp_int64(r, "total_sent", (int64_t)g_total_sent);
    resp_int64(r, "total_failed", (int64_t)g_total_failed);

    /* Text output */
    if (g_enabled) {
        char line[600];
        snprintf(line, sizeof(line), "URL: %s\n", g_url);
        resp_text_raw(r, line);
        snprintf(line, sizeof(line), "Secret: %s\n",
                 g_secret[0] ? "configured" : "none");
        resp_text_raw(r, line);
        snprintf(line, sizeof(line),
                 "Events: %d subscribed, Pending: %d\n",
                 g_sub_count, pending);
        resp_text_raw(r, line);
        snprintf(line, sizeof(line),
                 "Sent: %llu, Failed: %llu\n",
                 (unsigned long long)g_total_sent,
                 (unsigned long long)g_total_failed);
        resp_text_raw(r, line);
        snprintf(line, sizeof(line),
                 "Timeout: %dms, Retries: %d\n",
                 g_timeout_ms, g_retry_count);
        resp_text_raw(r, line);
    } else {
        resp_text_raw(r, "Webhook: disabled\n");
    }

    return 0;

usage:
    resp_text_raw(r, "Webhook notifications for repeater events\n\n"
        "  webhook\n"
        "    Show webhook status: URL, secret, subscribed events,\n"
        "    pending queue depth, total sent/failed counters.\n\n"
        "  webhook test\n"
        "    Enqueue a test payload to verify webhook delivery.\n\n"
        "    Fires HTTP POST with JSON payloads to a configured URL\n"
        "    when subscribed repeater events occur. Uses a background\n"
        "    worker thread with circular buffer queue to avoid blocking.\n"
        "    Supports X-Webhook-Secret header and configurable retries.\n\n"
        "    Available events: cor_assert, cor_drop, ptt_assert, ptt_drop,\n"
        "    caller_identified, caller_cleared, announcement,\n"
        "    recording_saved, state_change, shutdown, config_reload\n\n"
        "Config: [webhook] enabled, url, secret, events,\n"
        "        timeout_ms, retry_count\n");
    resp_str(r, "error", "usage: webhook [test]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "webhook", .usage = "webhook [test]",
      .description = "Webhook status or send test payload", .handler = cli_webhook, .category = "Control",
      .subcommands = "test" },
};

static kerchunk_module_def_t mod_webhook = {
    .name             = "mod_webhook",
    .version          = "1.0.0",
    .description      = "Webhook notifications for repeater events",
    .load             = webhook_load,
    .configure        = webhook_configure,
    .unload           = webhook_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_webhook);
