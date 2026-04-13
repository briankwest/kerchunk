/*
 * mod_ai.c — AI voice assistant (ASR → LLM → TTS)
 *
 * Consumes transcripts from mod_asr (KERCHEVT_ANNOUNCEMENT source="asr"),
 * decides whether the transcript is directed at the AI (wake phrase /
 * active conversation / DTMF arm), runs it through an OpenAI-compatible
 * /v1/chat/completions endpoint with tool-calling, and speaks the
 * final response via g_core->tts_speak().
 *
 * All LLM and HTTP work happens on a dedicated worker thread — the
 * audio thread and main loop are never blocked.
 *
 * Backends: Ollama, llama.cpp llama-server, vLLM, LM Studio, or any
 * OpenAI-compatible /v1/chat/completions endpoint.
 *
 * Config:   [ai] section in kerchunk.conf
 * Depends:  libcurl, libcjson, mod_asr, mod_tts
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>

#define LOG_MOD "ai"

/* Limits */
#define AI_MAX_TOOLS             32
#define AI_MAX_CONVERSATIONS     8
#define AI_MAX_MESSAGES_PER_CONV 20
#define AI_MAX_HISTORY           50   /* rolling log of Q → A for CLI */
#define AI_MAX_REQUEST_BYTES     65536
#define AI_MAX_RESPONSE_BYTES    (256 * 1024)
#define AI_MAX_PROMPT_BYTES      (64 * 1024)
#define AI_MAX_TOOL_RESULT_BYTES 4096
#define AI_MAX_MESSAGE_BYTES     2048
#define AI_REQUEST_QUEUE_SIZE    8

/* Request queue — ASR transcript hand-off from event thread to worker */
typedef struct {
    char transcript[AI_MAX_MESSAGE_BYTES];
    int  caller_id;
} ai_request_t;

/* Per-caller conversation state */
typedef struct {
    char role[16];              /* system / user / assistant / tool */
    char *content;              /* heap — may be NULL for assistant+tool_calls */
    char *tool_calls_json;      /* heap — raw JSON fragment for assistant tool_calls */
    char tool_call_id[64];      /* for role=tool */
} ai_message_t;

typedef struct {
    int    caller_id;           /* 0 = unused slot */
    time_t last_active;
    ai_message_t messages[AI_MAX_MESSAGES_PER_CONV];
    int    count;
} ai_conversation_t;

/* Tool registry entry */
typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json;   /* JSON Schema string, const */
    /* execute: args_json is the "arguments" field from the tool_call
     * (may be "{}"). Returns a malloc'd result string that the caller
     * must free. Must always return non-NULL. */
    char *(*execute)(const char *args_json, int caller_id, void *userdata);
    void *userdata;
    int   admin_only;
} ai_tool_t;

/* Recent history (for CLI `ai history`) */
typedef struct {
    time_t when;
    int    caller_id;
    char   question[256];
    char   answer[512];
    int    tool_rounds;
    int    ok;
} ai_history_entry_t;

/* ── Config ── */
static int   g_enabled;
static char  g_llm_url[512];
static char  g_model[128];
static char  g_api_key[256];
static int   g_timeout_s        = 30;
static int   g_verify_tls       = 1;
static int   g_max_tokens       = 200;
static double g_temperature     = 0.3;
static char  g_prompt_path[256] = "/etc/kerchunk/system_prompt.md";
static char *g_system_prompt;            /* heap, loaded from file */

/* Disable chain-of-thought "reasoning" in models that support it
 * (qwen3.x family). Appends "/no_think" to each user message so all
 * tokens go to content instead of a scratchpad the user never hears.
 * Harmless on non-reasoning models (they treat it as text).
 * Default ON — voice-over-radio never benefits from chain-of-thought,
 * and reasoning models otherwise blow the token budget on thinking. */
static int   g_disable_reasoning = 1;

typedef enum {
    AI_TRIGGER_WAKE_PHRASE,
    AI_TRIGGER_DTMF,
    AI_TRIGGER_ALWAYS,
} ai_trigger_mode_t;

static ai_trigger_mode_t g_trigger = AI_TRIGGER_WAKE_PHRASE;
static char  g_wake_phrase[64] = "kerchunk";
static int   g_conversation_timeout_s = 300;
static int   g_max_tool_rounds        = 3;

static int   g_standby_delay_ms = 2000;
static char  g_standby_cue[16]  = "sound";   /* sound | tts | none */
static char  g_standby_sound[128] = "system/standby";
static char  g_standby_text[128]  = "stand by";

static char  g_sound_offline[128] = "system/ai_offline";
static char  g_sound_error[128]   = "system/ai_error";
static char  g_sound_timeout[128] = "system/ai_timeout";

static int   g_max_consec_fail = 5;
static int   g_disable_after_s = 300;

/* ── State ── */
static kerchunk_core_t *g_core;

static ai_tool_t          g_tools[AI_MAX_TOOLS];
static int                g_tool_count;
static char               g_tools_json[16384];  /* pre-built tools array for requests */

static ai_conversation_t  g_convs[AI_MAX_CONVERSATIONS];
static pthread_mutex_t    g_convs_mutex = PTHREAD_MUTEX_INITIALIZER;

/* DTMF-armed state — next transmission goes to AI even without a wake
 * phrase. Set by *99#, cleared after first use. We need both a bool
 * flag and the caller id because an anonymous caller (id=0) would
 * otherwise be indistinguishable from "no arm active". */
static int                g_dtmf_armed;          /* 0/1 */
static int                g_dtmf_armed_caller;   /* caller id that dialed *99# (may be 0) */

/* Request queue (producer: on_announcement handler; consumer: worker) */
static ai_request_t       g_req_queue[AI_REQUEST_QUEUE_SIZE];
static int                g_req_head;
static int                g_req_tail;
static int                g_req_count;
static pthread_mutex_t    g_req_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t     g_req_cond  = PTHREAD_COND_INITIALIZER;
static int                g_worker_tid = -1;

/* History (for `ai history` CLI) */
static ai_history_entry_t g_history[AI_MAX_HISTORY];
static int                g_history_idx;
static int                g_history_count;
static pthread_mutex_t    g_history_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Stats + circuit breaker */
static uint64_t g_total_requests;
static uint64_t g_total_success;
static uint64_t g_total_failures;
static int      g_consec_failures;
static time_t   g_disabled_until;   /* 0 if not disabled */

/* ── Forward decls ── */
static void        conv_cleanup_expired(void);
static ai_conversation_t *conv_lookup_or_create(int caller_id);
static void        conv_append(ai_conversation_t *c, const char *role,
                                const char *content, const char *tool_calls_json,
                                const char *tool_call_id);
static void        conv_reset(ai_conversation_t *c);
static void        history_add(int caller_id, const char *q, const char *a,
                                int rounds, int ok);

/* ════════════════════════════════════════════════════════════════════
 *  System prompt loader
 * ════════════════════════════════════════════════════════════════════ */

static char *load_prompt_file(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0 || sz > AI_MAX_PROMPT_BYTES) { fclose(fp); return NULL; }
    rewind(fp);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = '\0';
    return buf;
}

static const char *default_system_prompt(void)
{
    return "You are a radio repeater assistant. Respond concisely — "
           "your output will be spoken over radio. Keep responses under "
           "three sentences. Use tools for real-time data.";
}

static void reload_system_prompt(void)
{
    char *newp = load_prompt_file(g_prompt_path);
    if (newp) {
        free(g_system_prompt);
        g_system_prompt = newp;
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "system prompt loaded: %s (%zu bytes)",
                    g_prompt_path, strlen(newp));
    } else {
        if (!g_system_prompt) {
            g_system_prompt = strdup(default_system_prompt());
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                        "system prompt file missing (%s) — using built-in default",
                        g_prompt_path);
        }
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  Conversation management
 * ════════════════════════════════════════════════════════════════════ */

static void conv_reset(ai_conversation_t *c)
{
    for (int i = 0; i < c->count; i++) {
        free(c->messages[i].content);
        free(c->messages[i].tool_calls_json);
    }
    memset(c, 0, sizeof(*c));
}

static void conv_cleanup_expired(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < AI_MAX_CONVERSATIONS; i++) {
        ai_conversation_t *c = &g_convs[i];
        if (c->caller_id == 0) continue;
        if (now - c->last_active > g_conversation_timeout_s)
            conv_reset(c);
    }
}

/* Caller 0 = anonymous/unknown — bucketed to slot 0 so anonymous users
 * still get multi-turn within the timeout. */
static ai_conversation_t *conv_lookup_or_create(int caller_id)
{
    conv_cleanup_expired();
    /* Existing */
    for (int i = 0; i < AI_MAX_CONVERSATIONS; i++) {
        if (g_convs[i].caller_id == caller_id && (caller_id != 0 || g_convs[i].count > 0))
            return &g_convs[i];
    }
    /* Free slot */
    for (int i = 0; i < AI_MAX_CONVERSATIONS; i++) {
        if (g_convs[i].caller_id == 0 && g_convs[i].count == 0) {
            g_convs[i].caller_id   = caller_id;
            g_convs[i].last_active = time(NULL);
            return &g_convs[i];
        }
    }
    /* Evict oldest */
    int oldest = 0;
    for (int i = 1; i < AI_MAX_CONVERSATIONS; i++) {
        if (g_convs[i].last_active < g_convs[oldest].last_active)
            oldest = i;
    }
    conv_reset(&g_convs[oldest]);
    g_convs[oldest].caller_id   = caller_id;
    g_convs[oldest].last_active = time(NULL);
    return &g_convs[oldest];
}

static void conv_append(ai_conversation_t *c, const char *role,
                        const char *content, const char *tool_calls_json,
                        const char *tool_call_id)
{
    /* Drop oldest user/assistant pair if full (keep room) */
    if (c->count >= AI_MAX_MESSAGES_PER_CONV) {
        free(c->messages[0].content);
        free(c->messages[0].tool_calls_json);
        for (int i = 1; i < c->count; i++)
            c->messages[i - 1] = c->messages[i];
        c->count--;
    }
    ai_message_t *m = &c->messages[c->count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->role, sizeof(m->role), "%s", role);
    if (content) m->content = strdup(content);
    if (tool_calls_json) m->tool_calls_json = strdup(tool_calls_json);
    if (tool_call_id) snprintf(m->tool_call_id, sizeof(m->tool_call_id), "%s", tool_call_id);
    c->last_active = time(NULL);
}

/* ════════════════════════════════════════════════════════════════════
 *  Tool registry
 * ════════════════════════════════════════════════════════════════════ */

static int tool_register(const char *name, const char *description,
                         const char *parameters_json,
                         char *(*execute)(const char *, int, void *),
                         void *userdata, int admin_only)
{
    if (g_tool_count >= AI_MAX_TOOLS) return -1;
    ai_tool_t *t = &g_tools[g_tool_count++];
    t->name            = name;
    t->description     = description;
    t->parameters_json = parameters_json;
    t->execute         = execute;
    t->userdata        = userdata;
    t->admin_only      = admin_only;
    return 0;
}

static ai_tool_t *tool_find(const char *name)
{
    if (!name) return NULL;
    for (int i = 0; i < g_tool_count; i++)
        if (strcmp(g_tools[i].name, name) == 0)
            return &g_tools[i];
    return NULL;
}

/* Build the tools JSON array once at configure() time. Returns length. */
static size_t build_tools_json(char *buf, size_t max)
{
    size_t j = 0;
    j += (size_t)snprintf(buf + j, max - j, "[");
    for (int i = 0; i < g_tool_count; i++) {
        ai_tool_t *t = &g_tools[i];
        if (i > 0) j += (size_t)snprintf(buf + j, max - j, ",");
        j += (size_t)snprintf(buf + j, max - j,
            "{\"type\":\"function\",\"function\":{"
            "\"name\":\"%s\",\"description\":\"%s\",\"parameters\":%s}}",
            t->name, t->description,
            t->parameters_json ? t->parameters_json
                               : "{\"type\":\"object\",\"properties\":{}}");
    }
    j += (size_t)snprintf(buf + j, max - j, "]");
    return j;
}

/* ════════════════════════════════════════════════════════════════════
 *  Built-in tool implementations
 *
 *  Pattern: run a kerchunk CLI command via kerchunk_dispatch_command()
 *  and return the JSON response, or synthesize a result directly.
 * ════════════════════════════════════════════════════════════════════ */

static char *cli_tool(const char *cmd_line)
{
    /* Tokenize cmd_line into argv (simple whitespace split, no quoting) */
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", cmd_line);
    const char *argv[8];
    int argc = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save);
         tok && argc < 8;
         tok = strtok_r(NULL, " \t", &save)) {
        argv[argc++] = tok;
    }
    if (argc == 0) return strdup("{\"error\":\"empty command\"}");

    kerchunk_resp_t resp;
    resp_init(&resp);
    kerchunk_dispatch_command(argc, argv, &resp);
    resp_finish(&resp);

    if (resp.jlen > 0 && resp.jlen < RESP_MAX)
        return strdup(resp.json);
    return strdup("{\"error\":\"dispatch failed\"}");
}

static char *tool_get_time(const char *args, int caller_id, void *ud)
{
    (void)args; (void)caller_id; (void)ud;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    tzset();
    const char *weekdays[] = {"Sunday","Monday","Tuesday","Wednesday",
                               "Thursday","Friday","Saturday"};
    char out[256];
    snprintf(out, sizeof(out),
        "{\"time\":\"%02d:%02d:%02d\",\"date\":\"%04d-%02d-%02d\","
        "\"weekday\":\"%s\",\"timezone\":\"%s\"}",
        tm.tm_hour, tm.tm_min, tm.tm_sec,
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        weekdays[tm.tm_wday],
        tzname[tm.tm_isdst > 0 ? 1 : 0] ? tzname[tm.tm_isdst > 0 ? 1 : 0] : "local");
    return strdup(out);
}

static char *tool_get_weather(const char *args, int caller_id, void *ud)
{
    (void)args; (void)caller_id; (void)ud;
    return cli_tool("weather");
}

static char *tool_get_forecast(const char *args, int caller_id, void *ud)
{
    (void)args; (void)caller_id; (void)ud;
    return cli_tool("weather forecast");
}

static char *tool_get_repeater_status(const char *args, int caller_id, void *ud)
{
    (void)args; (void)caller_id; (void)ud;
    return cli_tool("status");
}

static char *tool_get_stats(const char *args, int caller_id, void *ud)
{
    (void)args; (void)caller_id; (void)ud;
    return cli_tool("stats");
}

static char *tool_get_nws(const char *args, int caller_id, void *ud)
{
    (void)args; (void)caller_id; (void)ud;
    return cli_tool("nws");
}

static char *tool_get_user_info(const char *args, int caller_id, void *ud)
{
    (void)args; (void)ud;
    if (caller_id <= 0)
        return strdup("{\"identified\":false}");
    const kerchunk_user_t *u = g_core->user_lookup_by_id(caller_id);
    if (!u)
        return strdup("{\"identified\":false}");
    const char *access =
        u->access >= KERCHUNK_ACCESS_ADMIN ? "admin" :
        u->access >= KERCHUNK_ACCESS_BASIC ? "basic" : "none";
    char out[384];
    snprintf(out, sizeof(out),
        "{\"identified\":true,\"user_id\":%d,\"name\":\"%s\","
        "\"username\":\"%s\",\"callsign\":\"%s\",\"access\":\"%s\"}",
        caller_id, u->name, u->username, u->callsign, access);
    return strdup(out);
}

static char *tool_get_asr_history(const char *args, int caller_id, void *ud)
{
    (void)args; (void)caller_id; (void)ud;
    return cli_tool("asr history");
}

static char *tool_set_emergency(const char *args, int caller_id, void *ud)
{
    (void)caller_id; (void)ud;
    /* args: {"state":"on"|"off"} */
    cJSON *j = cJSON_Parse(args ? args : "{}");
    const char *state = "off";
    if (j) {
        cJSON *s = cJSON_GetObjectItem(j, "state");
        if (cJSON_IsString(s) && s->valuestring) state = s->valuestring;
    }
    int on = (strcasecmp(state, "on") == 0);
    kerchunk_core_set_emergency(on);
    if (j) cJSON_Delete(j);
    char out[96];
    snprintf(out, sizeof(out), "{\"emergency\":%s,\"action\":\"set\"}",
             on ? "true" : "false");
    return strdup(out);
}

static char *tool_send_page(const char *args, int caller_id, void *ud)
{
    (void)caller_id; (void)ud;
    /* args: {"message":"...","recipient":"..."} — for now we only
     * implement POCSAG/FLEX broadcast via CLI if available. A full
     * pager dispatch needs capcode lookup; defer. */
    cJSON *j = cJSON_Parse(args ? args : "{}");
    char *out = NULL;
    if (j) {
        cJSON *msg = cJSON_GetObjectItem(j, "message");
        if (cJSON_IsString(msg) && msg->valuestring) {
            /* Currently unsupported in this minimal implementation —
             * return a honest refusal the LLM can narrate. */
            out = strdup("{\"error\":\"send_page not wired in this build\"}");
        }
        cJSON_Delete(j);
    }
    if (!out)
        out = strdup("{\"error\":\"missing message\"}");
    return out;
}

static void register_builtin_tools(void)
{
    tool_register("get_time",
        "Get current date, time, day of week, and timezone.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_time, NULL, 0);

    tool_register("get_weather",
        "Get current weather conditions at the repeater site "
        "(temperature, wind, humidity, condition).",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_weather, NULL, 0);

    tool_register("get_forecast",
        "Get multi-day weather forecast for the repeater site.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_forecast, NULL, 0);

    tool_register("get_repeater_status",
        "Get the current repeater operational status: RX/TX state, "
        "uptime, COR active.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_repeater_status, NULL, 0);

    tool_register("get_stats",
        "Get repeater activity statistics: total RX count, TX count, "
        "top users, current duty cycle.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_stats, NULL, 0);

    tool_register("get_nws_alerts",
        "Get active NOAA National Weather Service alerts for the "
        "repeater site.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_nws, NULL, 0);

    tool_register("get_user_info",
        "Get information about the caller currently keying up "
        "(if identified via CTCSS, login, or OTP).",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_user_info, NULL, 0);

    tool_register("get_asr_history",
        "Get recent speech-to-text transcripts from transmissions "
        "on the repeater.",
        "{\"type\":\"object\",\"properties\":{}}",
        tool_get_asr_history, NULL, 0);

    tool_register("set_emergency",
        "Activate or deactivate repeater emergency mode. Requires "
        "admin authorization.",
        "{\"type\":\"object\",\"properties\":{"
        "\"state\":{\"type\":\"string\",\"enum\":[\"on\",\"off\"]}},"
        "\"required\":[\"state\"]}",
        tool_set_emergency, NULL, 1);

    tool_register("send_page",
        "Send a pager message to a configured recipient "
        "(POCSAG/FLEX). Requires admin authorization.",
        "{\"type\":\"object\",\"properties\":{"
        "\"recipient\":{\"type\":\"string\"},"
        "\"message\":{\"type\":\"string\"}},"
        "\"required\":[\"message\"]}",
        tool_send_page, NULL, 1);
}

/* ════════════════════════════════════════════════════════════════════
 *  JSON helpers for request construction
 * ════════════════════════════════════════════════════════════════════ */

static size_t json_escape(const char *in, char *out, size_t max)
{
    size_t j = 0;
    if (!in) { if (max) out[0] = '\0'; return 0; }
    for (const char *p = in; *p && j + 8 < max; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"':  out[j++] = '\\'; out[j++] = '"';  break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\n': out[j++] = '\\'; out[j++] = 'n';  break;
        case '\r': out[j++] = '\\'; out[j++] = 'r';  break;
        case '\t': out[j++] = '\\'; out[j++] = 't';  break;
        default:
            if (c < 0x20)
                j += (size_t)snprintf(out + j, max - j, "\\u%04x", c);
            else
                out[j++] = (char)c;
            break;
        }
    }
    out[j] = '\0';
    return j;
}

/* Append one message to the messages array buffer.
 * Handles: system, user, assistant (with optional tool_calls), tool. */
static size_t append_message(char *buf, size_t max, size_t j,
                             const ai_message_t *m, int first)
{
    if (!first) j += (size_t)snprintf(buf + j, max - j, ",");
    j += (size_t)snprintf(buf + j, max - j, "{\"role\":\"%s\"", m->role);
    if (m->content) {
        char *esc = malloc(strlen(m->content) * 6 + 4);
        if (esc) {
            json_escape(m->content, esc, strlen(m->content) * 6 + 4);
            j += (size_t)snprintf(buf + j, max - j, ",\"content\":\"%s\"", esc);
            free(esc);
        }
    } else {
        j += (size_t)snprintf(buf + j, max - j, ",\"content\":null");
    }
    if (m->tool_calls_json && m->tool_calls_json[0]) {
        j += (size_t)snprintf(buf + j, max - j, ",\"tool_calls\":%s",
                              m->tool_calls_json);
    }
    if (m->tool_call_id[0]) {
        j += (size_t)snprintf(buf + j, max - j, ",\"tool_call_id\":\"%s\"",
                              m->tool_call_id);
    }
    j += (size_t)snprintf(buf + j, max - j, "}");
    return j;
}

/* Build the full request JSON: system prompt + conversation messages +
 * tools. Returns length on success, 0 on overflow. */
static size_t build_request_json(const ai_conversation_t *c,
                                  int force_no_tools, char *buf, size_t max)
{
    size_t j = 0;

    /* Ollama honors the top-level "think" field on both /api/chat and
     * /v1/chat/completions — setting it false tells qwen3.x and other
     * reasoning models to skip chain-of-thought and put every token
     * into content. OpenAI and non-reasoning backends ignore the
     * unknown field. */
    j += (size_t)snprintf(buf + j, max - j,
        "{\"model\":\"%s\",\"max_tokens\":%d,\"temperature\":%.2f%s,\"messages\":[",
        g_model, g_max_tokens, g_temperature,
        g_disable_reasoning ? ",\"think\":false" : "");

    /* System message */
    {
        ai_message_t sys = {0};
        snprintf(sys.role, sizeof(sys.role), "system");
        sys.content = g_system_prompt ? g_system_prompt : (char *)default_system_prompt();
        j = append_message(buf, max, j, &sys, 1);
        /* don't free — g_system_prompt is module-owned; default is const */
    }

    /* Conversation messages */
    for (int i = 0; i < c->count; i++)
        j = append_message(buf, max, j, &c->messages[i], 0);

    j += (size_t)snprintf(buf + j, max - j, "]");

    /* Tools + tool_choice */
    j += (size_t)snprintf(buf + j, max - j, ",\"tools\":%s", g_tools_json);
    j += (size_t)snprintf(buf + j, max - j, ",\"tool_choice\":\"%s\"",
                          force_no_tools ? "none" : "auto");

    j += (size_t)snprintf(buf + j, max - j, "}");

    if (j >= max - 1) return 0;  /* overflow */
    return j;
}

/* ════════════════════════════════════════════════════════════════════
 *  libcurl POST
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} response_buf_t;

static size_t curl_write_cb(char *ptr, size_t sz, size_t nm, void *ud)
{
    response_buf_t *rb = (response_buf_t *)ud;
    size_t add = sz * nm;
    if (rb->len + add + 1 > rb->cap) {
        size_t new_cap = rb->cap ? rb->cap * 2 : 8192;
        while (new_cap < rb->len + add + 1) new_cap *= 2;
        if (new_cap > AI_MAX_RESPONSE_BYTES) {
            /* Cap — further bytes dropped */
            return 0;
        }
        char *nb = realloc(rb->data, new_cap);
        if (!nb) return 0;
        rb->data = nb;
        rb->cap  = new_cap;
    }
    memcpy(rb->data + rb->len, ptr, add);
    rb->len += add;
    rb->data[rb->len] = '\0';
    return add;
}

typedef enum {
    AI_POST_OK = 0,
    AI_POST_TIMEOUT,
    AI_POST_CONNECT,
    AI_POST_HTTP_ERR,
    AI_POST_AUTH,
    AI_POST_NOT_FOUND,
    AI_POST_PARSE,
    AI_POST_INTERNAL,
} ai_post_result_t;

static ai_post_result_t post_to_llm(const char *body, response_buf_t *rb)
{
    CURL *curl = curl_easy_init();
    if (!curl) return AI_POST_INTERNAL;

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (g_api_key[0]) {
        char h[320];
        snprintf(h, sizeof(h), "Authorization: Bearer %s", g_api_key);
        headers = curl_slist_append(headers, h);
    }

    char ua[128];
    snprintf(ua, sizeof(ua), "kerchunkd/" KERCHUNK_VERSION_STRING " mod_ai");

    curl_easy_setopt(curl, CURLOPT_URL, g_llm_url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)g_timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, g_verify_tls ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, g_verify_tls ? 2L : 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, rb);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc == CURLE_OPERATION_TIMEDOUT) return AI_POST_TIMEOUT;
    if (rc == CURLE_COULDNT_CONNECT || rc == CURLE_COULDNT_RESOLVE_HOST)
        return AI_POST_CONNECT;
    if (rc != CURLE_OK) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "curl: %s",
                    curl_easy_strerror(rc));
        return AI_POST_HTTP_ERR;
    }
    if (http_code == 401 || http_code == 403) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
            "HTTP %ld — request: %s", http_code, body);
        if (rb->data)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "HTTP %ld — response: %s", http_code, rb->data);
        return AI_POST_AUTH;
    }
    if (http_code == 404) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
            "HTTP 404 — request: %s", body);
        if (rb->data)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "HTTP 404 — response: %s", rb->data);
        return AI_POST_NOT_FOUND;
    }
    if (http_code < 200 || http_code >= 300) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
            "HTTP %ld — request: %s", http_code, body);
        if (rb->data)
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "HTTP %ld — response: %s", http_code, rb->data);
        return AI_POST_HTTP_ERR;
    }
    /* Success — leave a DEBUG trail for post-mortem of good responses */
    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "LLM request:  %s", body);
    if (rb->data)
        g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD, "LLM response: %s", rb->data);
    return AI_POST_OK;
}

/* ════════════════════════════════════════════════════════════════════
 *  Parse LLM response
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    char *finish_reason;        /* "stop" / "tool_calls" / ... */
    char *content;              /* heap, may be NULL */
    char *tool_calls_json;      /* heap, raw JSON of the tool_calls array */
} ai_reply_t;

static void ai_reply_free(ai_reply_t *r)
{
    free(r->finish_reason);
    free(r->content);
    free(r->tool_calls_json);
    memset(r, 0, sizeof(*r));
}

static int parse_llm_response(const char *body, ai_reply_t *out)
{
    memset(out, 0, sizeof(*out));
    cJSON *root = cJSON_Parse(body);
    if (!root) return -1;

    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root); return -1;
    }
    cJSON *c0 = cJSON_GetArrayItem(choices, 0);
    if (!c0) { cJSON_Delete(root); return -1; }

    cJSON *fr = cJSON_GetObjectItem(c0, "finish_reason");
    if (cJSON_IsString(fr) && fr->valuestring)
        out->finish_reason = strdup(fr->valuestring);

    cJSON *msg = cJSON_GetObjectItem(c0, "message");
    if (!msg) { cJSON_Delete(root); return -1; }

    cJSON *content = cJSON_GetObjectItem(msg, "content");
    if (cJSON_IsString(content) && content->valuestring && content->valuestring[0])
        out->content = strdup(content->valuestring);

    cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
    if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
        char *printed = cJSON_PrintUnformatted(tool_calls);
        if (printed) { out->tool_calls_json = printed; }
    }

    cJSON_Delete(root);
    return 0;
}

/* Strip control chars, markdown fencing, and backticks from the final
 * spoken response — TTS doesn't want asterisks or code fences. */
static void sanitize_for_tts(char *s)
{
    if (!s) return;
    char *w = s;
    for (char *r = s; *r; r++) {
        char c = *r;
        if (c == '*' || c == '`' || c == '#' || c == '_' || c == '~')
            continue;
        if ((unsigned char)c < 0x20 && c != ' ') continue;
        *w++ = c;
    }
    *w = '\0';
}

/* ════════════════════════════════════════════════════════════════════
 *  Tool execution (handles tool_calls array from an LLM reply)
 * ════════════════════════════════════════════════════════════════════ */

/* Execute every tool_call in the array and append tool-role messages
 * to the conversation with the results. Returns number of calls
 * executed (0 = nothing to do). */
static int execute_tool_calls(const char *tool_calls_json,
                              ai_conversation_t *c, int caller_id,
                              int caller_is_admin)
{
    cJSON *arr = cJSON_Parse(tool_calls_json);
    if (!arr || !cJSON_IsArray(arr)) { if (arr) cJSON_Delete(arr); return 0; }

    int n = 0;
    int sz = cJSON_GetArraySize(arr);
    for (int i = 0; i < sz; i++) {
        cJSON *tc = cJSON_GetArrayItem(arr, i);
        if (!tc) continue;
        cJSON *id_j = cJSON_GetObjectItem(tc, "id");
        cJSON *fn   = cJSON_GetObjectItem(tc, "function");
        if (!fn) continue;
        cJSON *name_j = cJSON_GetObjectItem(fn, "name");
        cJSON *args_j = cJSON_GetObjectItem(fn, "arguments");

        const char *id   = (cJSON_IsString(id_j) && id_j->valuestring)
                             ? id_j->valuestring : "call_0";
        const char *name = (cJSON_IsString(name_j) && name_j->valuestring)
                             ? name_j->valuestring : "";
        const char *args = NULL;
        char args_buf[AI_MAX_TOOL_RESULT_BYTES];
        args_buf[0] = '\0';
        if (cJSON_IsString(args_j) && args_j->valuestring) {
            args = args_j->valuestring;
        } else if (args_j) {
            /* Some backends embed arguments as an object — serialize */
            char *p = cJSON_PrintUnformatted(args_j);
            if (p) {
                snprintf(args_buf, sizeof(args_buf), "%s", p);
                free(p);
                args = args_buf;
            }
        }
        if (!args) args = "{}";

        ai_tool_t *tool = tool_find(name);
        char *result = NULL;
        if (!tool) {
            char err[160];
            snprintf(err, sizeof(err),
                     "{\"error\":\"unknown tool: %s\"}", name);
            result = strdup(err);
        } else if (tool->admin_only && !caller_is_admin) {
            result = strdup("{\"error\":\"access denied — admin required\"}");
        } else {
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "tool call: %s(%s)", name, args);
            result = tool->execute(args, caller_id, tool->userdata);
            if (!result) result = strdup("{\"error\":\"tool returned null\"}");
        }

        /* Cap tool result size so we don't blow the request buffer */
        if (result && strlen(result) > AI_MAX_TOOL_RESULT_BYTES - 64) {
            char *trunc = malloc(AI_MAX_TOOL_RESULT_BYTES);
            if (trunc) {
                snprintf(trunc, AI_MAX_TOOL_RESULT_BYTES,
                         "%.*s\"...truncated\"}",
                         AI_MAX_TOOL_RESULT_BYTES - 64, result);
                free(result);
                result = trunc;
            }
        }

        conv_append(c, "tool", result, NULL, id);
        free(result);
        n++;
    }
    cJSON_Delete(arr);
    return n;
}

/* ════════════════════════════════════════════════════════════════════
 *  Orchestration — run one full request/response cycle
 * ════════════════════════════════════════════════════════════════════ */

static int caller_is_admin_p(int caller_id)
{
    if (caller_id <= 0) return 0;
    if (kerchunk_core_get_otp_elevated(caller_id)) return 1;
    const kerchunk_user_t *u = g_core->user_lookup_by_id(caller_id);
    if (u && u->access >= KERCHUNK_ACCESS_ADMIN) return 1;
    return 0;
}

/* Returns 0 on success and stores final assistant text in `out_text`
 * (heap-alloc, caller frees). Returns -1 on error. */
static int run_ai_turn(ai_conversation_t *c, int caller_id, char **out_text,
                       int *out_rounds)
{
    *out_text = NULL;
    *out_rounds = 0;

    int caller_admin = caller_is_admin_p(caller_id);

    char *body = malloc(AI_MAX_REQUEST_BYTES);
    if (!body) return -1;

    int rounds = 0;
    int force_no_tools = 0;

    for (;;) {
        size_t n = build_request_json(c, force_no_tools, body, AI_MAX_REQUEST_BYTES);
        if (n == 0) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                        "request JSON overflow — conversation too large");
            free(body);
            return -1;
        }

        response_buf_t rb = {0};
        ai_post_result_t pr = post_to_llm(body, &rb);

        if (pr != AI_POST_OK) {
            free(rb.data);
            const char *wav = g_sound_error;
            const char *fallback = "The assistant is unavailable right now.";
            switch (pr) {
            case AI_POST_TIMEOUT:
                wav = g_sound_timeout;
                fallback = "The assistant took too long to respond.";
                g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "LLM timeout");
                break;
            case AI_POST_CONNECT:
                wav = g_sound_offline;
                fallback = "The assistant is offline.";
                g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "LLM connect failed");
                break;
            case AI_POST_AUTH:
                fallback = "The assistant authentication failed.";
                g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "LLM auth failed (bad api_key?)");
                break;
            case AI_POST_NOT_FOUND:
                fallback = "The assistant model is not loaded.";
                g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "LLM 404 — model missing?");
                break;
            default:
                g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "LLM HTTP error");
                break;
            }
            free(body);
            /* Try to play the pre-recorded WAV; otherwise fall back to TTS */
            if (g_core->queue_audio_file(wav, KERCHUNK_PRI_NORMAL) != 0
                && g_core->tts_speak)
                g_core->tts_speak(fallback, KERCHUNK_PRI_NORMAL);
            return -1;
        }

        ai_reply_t reply;
        int prc = parse_llm_response(rb.data, &reply);
        if (prc != 0) {
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "parse failed; body: %s",
                        rb.data ? rb.data : "(empty)");
            free(rb.data);
            free(body);
            if (g_core->tts_speak)
                g_core->tts_speak("The assistant returned a malformed response.",
                                  KERCHUNK_PRI_NORMAL);
            return -1;
        }

        /* Keep the raw response around for one more failure path (empty
         * content AND no tool_calls → we want to see what the model sent) */
        char *raw_body_copy = rb.data ? strdup(rb.data) : NULL;
        free(rb.data);

        rounds++;

        /* Append assistant message to conversation */
        conv_append(c, "assistant", reply.content, reply.tool_calls_json, NULL);

        if (reply.tool_calls_json && !force_no_tools &&
            rounds < g_max_tool_rounds) {
            int executed = execute_tool_calls(reply.tool_calls_json, c,
                                              caller_id, caller_admin);
            ai_reply_free(&reply);
            free(raw_body_copy);
            if (executed == 0) {
                /* Nothing executed (parse trouble); force a plain answer */
                force_no_tools = 1;
            }
            continue;
        }

        if (reply.tool_calls_json && !force_no_tools) {
            /* Hit round limit — force one more call with tool_choice none */
            force_no_tools = 1;
            ai_reply_free(&reply);
            free(raw_body_copy);
            continue;
        }

        /* Final text response */
        if (reply.content && reply.content[0]) {
            *out_text = strdup(reply.content);
            sanitize_for_tts(*out_text);
        } else {
            /* Empty content on final round — dump what we actually got
             * so we can see why the model went silent. This is the
             * failure signature that points at "too small for tool use". */
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                "empty content on round %d; finish_reason=%s; body: %s",
                rounds,
                reply.finish_reason ? reply.finish_reason : "(none)",
                raw_body_copy ? raw_body_copy : "(lost)");
            *out_text = strdup("The assistant did not provide a response.");
        }
        *out_rounds = rounds;
        ai_reply_free(&reply);
        free(raw_body_copy);
        free(body);
        return 0;
    }
}

/* ════════════════════════════════════════════════════════════════════
 *  Wake phrase detection / trigger
 * ════════════════════════════════════════════════════════════════════ */

/* Case-insensitive prefix check; on match, return the text-after-phrase
 * pointer (may be empty). Return NULL on no match. */
static const char *strip_wake_phrase(const char *text)
{
    if (!g_wake_phrase[0]) return NULL;
    size_t wlen = strlen(g_wake_phrase);
    const char *p = text;
    while (*p && isspace((unsigned char)*p)) p++;
    if (strncasecmp(p, g_wake_phrase, wlen) != 0) return NULL;
    /* Must be followed by whitespace, punctuation, or end of string */
    char next = p[wlen];
    if (next && !isspace((unsigned char)next) && !ispunct((unsigned char)next))
        return NULL;
    p += wlen;
    while (*p && (isspace((unsigned char)*p) || ispunct((unsigned char)*p)))
        p++;
    return p;
}

/* ════════════════════════════════════════════════════════════════════
 *  Worker thread
 * ════════════════════════════════════════════════════════════════════ */

static int enqueue_request(const char *transcript, int caller_id)
{
    pthread_mutex_lock(&g_req_mutex);
    if (g_req_count >= AI_REQUEST_QUEUE_SIZE) {
        pthread_mutex_unlock(&g_req_mutex);
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "request queue full, dropping");
        return -1;
    }
    ai_request_t *r = &g_req_queue[g_req_tail];
    snprintf(r->transcript, sizeof(r->transcript), "%s", transcript);
    r->caller_id = caller_id;
    g_req_tail = (g_req_tail + 1) % AI_REQUEST_QUEUE_SIZE;
    g_req_count++;
    pthread_cond_signal(&g_req_cond);
    pthread_mutex_unlock(&g_req_mutex);
    return 0;
}

static void *ai_worker(void *arg)
{
    (void)arg;

    while (!g_core->thread_should_stop(g_worker_tid)) {
        ai_request_t req;

        pthread_mutex_lock(&g_req_mutex);
        while (g_req_count == 0 && !g_core->thread_should_stop(g_worker_tid)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&g_req_cond, &g_req_mutex, &ts);
        }
        if (g_core->thread_should_stop(g_worker_tid)) {
            pthread_mutex_unlock(&g_req_mutex);
            break;
        }
        req = g_req_queue[g_req_head];
        g_req_head = (g_req_head + 1) % AI_REQUEST_QUEUE_SIZE;
        g_req_count--;
        pthread_mutex_unlock(&g_req_mutex);

        /* Circuit breaker */
        if (g_disabled_until && time(NULL) < g_disabled_until) continue;
        if (g_disabled_until && time(NULL) >= g_disabled_until) {
            g_disabled_until = 0;
            g_consec_failures = 0;
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "AI re-enabled after cooldown");
        }

        g_total_requests++;

        /* Build/append to conversation */
        pthread_mutex_lock(&g_convs_mutex);
        ai_conversation_t *c = conv_lookup_or_create(req.caller_id);
        conv_append(c, "user", req.transcript, NULL, NULL);

        char *answer = NULL;
        int rounds = 0;
        int ok = run_ai_turn(c, req.caller_id, &answer, &rounds) == 0;
        pthread_mutex_unlock(&g_convs_mutex);

        if (ok && answer && answer[0]) {
            g_total_success++;
            g_consec_failures = 0;
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                        "answer (rounds=%d): %s", rounds, answer);
            if (g_core->tts_speak)
                g_core->tts_speak(answer, KERCHUNK_PRI_ELEVATED);
            history_add(req.caller_id, req.transcript, answer, rounds, 1);
        } else {
            g_total_failures++;
            g_consec_failures++;
            history_add(req.caller_id, req.transcript,
                        answer ? answer : "(failed)",
                        rounds, 0);
            if (g_max_consec_fail > 0 &&
                g_consec_failures >= g_max_consec_fail) {
                g_disabled_until = time(NULL) + g_disable_after_s;
                g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                            "AI disabled for %ds after %d consecutive failures",
                            g_disable_after_s, g_consec_failures);
                if (g_core->tts_speak)
                    g_core->tts_speak("Assistant disabled.",
                                      KERCHUNK_PRI_NORMAL);
            }
        }
        free(answer);
    }
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════
 *  History
 * ════════════════════════════════════════════════════════════════════ */

static void history_add(int caller_id, const char *q, const char *a,
                        int rounds, int ok)
{
    pthread_mutex_lock(&g_history_mutex);
    ai_history_entry_t *e = &g_history[g_history_idx];
    e->when        = time(NULL);
    e->caller_id   = caller_id;
    e->tool_rounds = rounds;
    e->ok          = ok;
    snprintf(e->question, sizeof(e->question), "%s", q ? q : "");
    snprintf(e->answer,   sizeof(e->answer),   "%s", a ? a : "");
    g_history_idx = (g_history_idx + 1) % AI_MAX_HISTORY;
    if (g_history_count < AI_MAX_HISTORY) g_history_count++;
    pthread_mutex_unlock(&g_history_mutex);
}

/* ════════════════════════════════════════════════════════════════════
 *  Event handlers
 * ════════════════════════════════════════════════════════════════════ */

/* Current caller — updated by CALLER_IDENTIFIED / CALLER_CLEARED */
static int g_current_caller_id;

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_current_caller_id = evt->caller.user_id;
}

static void on_caller_cleared(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_current_caller_id = 0;
}

static void on_announcement(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_enabled) return;
    if (!evt->announcement.source) return;
    if (strcmp(evt->announcement.source, "asr") != 0) return;

    const char *transcript = evt->announcement.description;
    if (!transcript || !transcript[0]) return;

    int caller_id = g_current_caller_id;
    const char *to_send = transcript;
    int trigger_hit = 0;

    /* 1) Always mode — every transcript goes through */
    if (g_trigger == AI_TRIGGER_ALWAYS) {
        trigger_hit = 1;
    }

    /* 2) DTMF *99# arm — consume if it matches this caller. A
     * caller-specific arm (armed_caller != 0) only fires for that
     * caller's next TX; an anonymous arm (armed_caller == 0) consumes
     * on the next TX from anyone. Mismatched callers leave the arm
     * in place and fall through to the wake-phrase check. */
    if (!trigger_hit && g_dtmf_armed) {
        int match = (g_dtmf_armed_caller == 0) ||
                    (caller_id != 0 && g_dtmf_armed_caller == caller_id);
        if (match) {
            g_dtmf_armed = 0;
            g_dtmf_armed_caller = 0;
            trigger_hit = 1;
        }
    }

    /* 3) Wake phrase + conversation continuation */
    if (!trigger_hit) {
        const char *after = strip_wake_phrase(transcript);
        if (after && *after) {
            to_send = after;
            trigger_hit = 1;
        } else if (after && !*after) {
            /* Wake phrase with nothing after — acknowledge, nothing to do */
            if (g_core->tts_speak)
                g_core->tts_speak("Ready.", KERCHUNK_PRI_NORMAL);
            return;
        } else {
            /* No wake phrase — pass through only if this caller has
             * an active conversation from a prior turn. */
            pthread_mutex_lock(&g_convs_mutex);
            conv_cleanup_expired();
            for (int i = 0; i < AI_MAX_CONVERSATIONS; i++) {
                if (g_convs[i].caller_id == caller_id &&
                    g_convs[i].count > 0) {
                    trigger_hit = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&g_convs_mutex);
        }
    }

    if (!trigger_hit) return;  /* not for us */

    enqueue_request(to_send, caller_id);
}

/* DTMF *99<arg># — single registration, dispatch on the trailing arg:
 *
 *   *99#    (arg empty) → arm AI for this caller's next transmission
 *   *990#   (arg "0")   → stop AI: cancel arm + clear this caller's
 *                          conversation. Per-caller stop, not a global
 *                          kill switch — the [ai] enabled flag is
 *                          untouched.
 *
 * Follows the same single-registration pattern as mod_scrambler's *97#
 * family. Registering both "99" and "990" would break because
 * mod_dtmfcmd does first-match dispatch — "99" would intercept every
 * *990# before the "990" handler got a chance. */
static void on_dtmf_ai(const kerchevt_t *evt, void *ud)
{
    (void)ud;

    /* Extract the trailing digits (everything after the "99" prefix). */
    char arg[8] = "";
    if (evt->custom.data && evt->custom.len > 0) {
        size_t n = evt->custom.len > 7 ? 7 : evt->custom.len;
        memcpy(arg, evt->custom.data, n);
        arg[n] = '\0';
    }

    if (arg[0] == '\0') {
        /* *99# — arm */
        g_dtmf_armed        = 1;
        g_dtmf_armed_caller = g_current_caller_id;
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "AI armed for next TX (caller=%d)", g_dtmf_armed_caller);
        if (g_core->tts_speak)
            g_core->tts_speak("Assistant ready.", KERCHUNK_PRI_NORMAL);
        return;
    }

    if (strcmp(arg, "0") == 0) {
        /* *990# — stop */
        int caller_id = g_current_caller_id;
        int had_arm   = g_dtmf_armed;
        int cleared_convs = 0;

        g_dtmf_armed        = 0;
        g_dtmf_armed_caller = 0;

        pthread_mutex_lock(&g_convs_mutex);
        for (int i = 0; i < AI_MAX_CONVERSATIONS; i++) {
            if (g_convs[i].caller_id == caller_id && g_convs[i].count > 0) {
                conv_reset(&g_convs[i]);
                cleared_convs++;
            }
        }
        pthread_mutex_unlock(&g_convs_mutex);

        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "AI stopped (caller=%d, had_arm=%d, cleared_convs=%d)",
                    caller_id, had_arm, cleared_convs);

        if (g_core->tts_speak)
            g_core->tts_speak("Assistant stopped.", KERCHUNK_PRI_NORMAL);
        return;
    }

    /* Unknown *99<arg># — ignore with a debug log */
    g_core->log(KERCHUNK_LOG_DEBUG, LOG_MOD,
                "ignoring unknown *99%s# arg", arg);
}

/* ════════════════════════════════════════════════════════════════════
 *  Module lifecycle
 * ════════════════════════════════════════════════════════════════════ */

/* Custom-event offsets. Must not collide with other modules:
 *   16 = mod_scrambler (*97#)
 *   17 = mod_freeswitch autopatch (*0<digits>#)
 * 18 is mod_ai's slot. The *99# and *990# pair ships as a single
 * registration — the handler parses the trailing argument. */
#define DTMF_AI_OFFSET 18   /* KERCHEVT_CUSTOM + 18 — *99 prefix */

static int ai_load(kerchunk_core_t *core)
{
    g_core = core;
    register_builtin_tools();
    build_tools_json(g_tools_json, sizeof(g_tools_json));
    return 0;
}

static int ai_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "ai", "enabled");
    g_enabled = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "ai", "llm_url");
    if (v) snprintf(g_llm_url, sizeof(g_llm_url), "%s", v);

    v = kerchunk_config_get(cfg, "ai", "llm_model");
    if (v) snprintf(g_model, sizeof(g_model), "%s", v);

    v = kerchunk_config_get(cfg, "ai", "llm_api_key");
    if (v) snprintf(g_api_key, sizeof(g_api_key), "%s", v);
    else g_api_key[0] = '\0';

    g_timeout_s = kerchunk_config_get_int(cfg, "ai", "llm_timeout_s", 30);

    v = kerchunk_config_get(cfg, "ai", "llm_verify_tls");
    g_verify_tls = (!v || strcmp(v, "off") != 0);

    g_max_tokens  = kerchunk_config_get_int(cfg, "ai", "max_tokens", 200);
    {
        v = kerchunk_config_get(cfg, "ai", "temperature");
        if (v) g_temperature = atof(v);
    }

    /* disable_reasoning — default on. Only "off" disables it. */
    v = kerchunk_config_get(cfg, "ai", "disable_reasoning");
    g_disable_reasoning = (!v || strcmp(v, "off") != 0);

    v = kerchunk_config_get(cfg, "ai", "system_prompt_file");
    if (v) snprintf(g_prompt_path, sizeof(g_prompt_path), "%s", v);
    reload_system_prompt();

    v = kerchunk_config_get(cfg, "ai", "trigger");
    if (v) {
        if (strcmp(v, "always") == 0)     g_trigger = AI_TRIGGER_ALWAYS;
        else if (strcmp(v, "dtmf") == 0)  g_trigger = AI_TRIGGER_DTMF;
        else                              g_trigger = AI_TRIGGER_WAKE_PHRASE;
    }

    v = kerchunk_config_get(cfg, "ai", "wake_phrase");
    if (v) snprintf(g_wake_phrase, sizeof(g_wake_phrase), "%s", v);

    g_conversation_timeout_s =
        kerchunk_config_get_duration_ms(cfg, "ai", "conversation_timeout", 300000) / 1000;
    g_max_tool_rounds =
        kerchunk_config_get_int(cfg, "ai", "max_tool_rounds", 3);

    g_standby_delay_ms =
        kerchunk_config_get_duration_ms(cfg, "ai", "standby_delay_ms", 2000);
    v = kerchunk_config_get(cfg, "ai", "standby_cue");
    if (v) snprintf(g_standby_cue, sizeof(g_standby_cue), "%s", v);
    v = kerchunk_config_get(cfg, "ai", "standby_sound");
    if (v) snprintf(g_standby_sound, sizeof(g_standby_sound), "%s", v);
    v = kerchunk_config_get(cfg, "ai", "standby_text");
    if (v) snprintf(g_standby_text, sizeof(g_standby_text), "%s", v);

    v = kerchunk_config_get(cfg, "ai", "sound_offline");
    if (v) snprintf(g_sound_offline, sizeof(g_sound_offline), "%s", v);
    v = kerchunk_config_get(cfg, "ai", "sound_error");
    if (v) snprintf(g_sound_error, sizeof(g_sound_error), "%s", v);
    v = kerchunk_config_get(cfg, "ai", "sound_timeout");
    if (v) snprintf(g_sound_timeout, sizeof(g_sound_timeout), "%s", v);

    g_max_consec_fail =
        kerchunk_config_get_int(cfg, "ai", "max_consecutive_failures", 5);
    g_disable_after_s =
        kerchunk_config_get_duration_ms(cfg, "ai", "disable_after_fail_s", 300000) / 1000;

    if (!g_enabled) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "disabled");
        return 0;
    }

    if (!g_llm_url[0]) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "enabled but llm_url not set — disabling");
        g_enabled = 0;
        return 0;
    }
    if (!g_model[0]) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "llm_model not set — backends requiring model name will fail");
    }

    /* Subscribe event handlers (unsubscribe first for config reload) */
    g_core->unsubscribe(KERCHEVT_ANNOUNCEMENT, on_announcement);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_CALLER_CLEARED, on_caller_cleared);
    g_core->unsubscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + DTMF_AI_OFFSET),
                         on_dtmf_ai);

    g_core->subscribe(KERCHEVT_ANNOUNCEMENT, on_announcement, NULL);
    g_core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    g_core->subscribe(KERCHEVT_CALLER_CLEARED, on_caller_cleared, NULL);

    /* Register *99<arg># — single pattern, the handler dispatches on
     * the trailing arg: empty = arm, "0" = stop. Same approach as
     * mod_scrambler's *97<arg># family. */
    if (g_core->dtmf_register) {
        g_core->dtmf_register("99", DTMF_AI_OFFSET,
                              "AI assistant (*99# arm, *990# stop)",
                              "dtmf_ai");
        g_core->subscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + DTMF_AI_OFFSET),
                           on_dtmf_ai, NULL);
    }

    /* Start worker thread if not already running */
    if (g_worker_tid < 0) {
        g_worker_tid = g_core->thread_create("ai-worker", ai_worker, NULL);
        if (g_worker_tid < 0) {
            g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create worker");
            g_enabled = 0;
            return 0;
        }
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "ready: url=%s model=%s trigger=%s wake=\"%s\" tools=%d",
                g_llm_url, g_model,
                g_trigger == AI_TRIGGER_WAKE_PHRASE ? "wake_phrase" :
                g_trigger == AI_TRIGGER_DTMF ? "dtmf" : "always",
                g_wake_phrase, g_tool_count);
    return 0;
}

static void ai_unload(void)
{
    g_enabled = 0;

    if (g_worker_tid >= 0) {
        g_core->thread_stop(g_worker_tid);
        pthread_cond_signal(&g_req_cond);
        g_core->thread_join(g_worker_tid);
        g_worker_tid = -1;
    }

    g_core->unsubscribe(KERCHEVT_ANNOUNCEMENT, on_announcement);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_CALLER_CLEARED, on_caller_cleared);
    g_core->unsubscribe((kerchevt_type_t)(KERCHEVT_CUSTOM + DTMF_AI_OFFSET),
                         on_dtmf_ai);

    pthread_mutex_lock(&g_convs_mutex);
    for (int i = 0; i < AI_MAX_CONVERSATIONS; i++)
        conv_reset(&g_convs[i]);
    pthread_mutex_unlock(&g_convs_mutex);

    free(g_system_prompt);
    g_system_prompt = NULL;
}

/* ════════════════════════════════════════════════════════════════════
 *  CLI commands
 * ════════════════════════════════════════════════════════════════════ */

static int cli_ai(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;

    /* ai tools — list registered tools */
    if (argc >= 2 && strcmp(argv[1], "tools") == 0) {
        resp_int(r, "count", g_tool_count);
        resp_json_raw(r, ",\"tools\":[");
        for (int i = 0; i < g_tool_count; i++) {
            ai_tool_t *t = &g_tools[i];
            char frag[512];
            snprintf(frag, sizeof(frag),
                "%s{\"name\":\"%s\",\"description\":\"%s\",\"admin_only\":%s}",
                i > 0 ? "," : "",
                t->name,
                t->description ? t->description : "",
                t->admin_only ? "true" : "false");
            resp_json_raw(r, frag);
            char line[256];
            snprintf(line, sizeof(line), "  %s%s — %s\n",
                     t->name,
                     t->admin_only ? " (admin)" : "",
                     t->description ? t->description : "");
            resp_text_raw(r, line);
        }
        resp_json_raw(r, "]");
        return 0;
    }

    /* ai history — show recent Q/A */
    if (argc >= 2 && strcmp(argv[1], "history") == 0) {
        pthread_mutex_lock(&g_history_mutex);
        resp_int(r, "count", g_history_count);
        int start = (g_history_idx - g_history_count + AI_MAX_HISTORY) % AI_MAX_HISTORY;
        for (int k = 0; k < g_history_count; k++) {
            ai_history_entry_t *e = &g_history[(start + k) % AI_MAX_HISTORY];
            char line[1024];
            struct tm tm;
            localtime_r(&e->when, &tm);
            snprintf(line, sizeof(line),
                "[%02d:%02d:%02d] caller=%d rounds=%d %s\n  Q: %s\n  A: %s\n",
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                e->caller_id, e->tool_rounds,
                e->ok ? "OK" : "FAIL",
                e->question, e->answer);
            resp_text_raw(r, line);
        }
        pthread_mutex_unlock(&g_history_mutex);
        return 0;
    }

    /* ai reset — clear all conversations */
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        pthread_mutex_lock(&g_convs_mutex);
        for (int i = 0; i < AI_MAX_CONVERSATIONS; i++)
            conv_reset(&g_convs[i]);
        pthread_mutex_unlock(&g_convs_mutex);
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "all conversations cleared");
        return 0;
    }

    /* ai ask <text> — manual trigger bypassing wake phrase */
    if (argc >= 3 && strcmp(argv[1], "ask") == 0) {
        /* Reconstitute argv[2..] into a single string */
        char text[AI_MAX_MESSAGE_BYTES];
        size_t pos = 0;
        for (int i = 2; i < argc && pos < sizeof(text) - 2; i++) {
            if (i > 2) text[pos++] = ' ';
            size_t l = strlen(argv[i]);
            if (pos + l >= sizeof(text)) l = sizeof(text) - pos - 1;
            memcpy(text + pos, argv[i], l);
            pos += l;
        }
        text[pos] = '\0';

        if (enqueue_request(text, g_current_caller_id) == 0) {
            resp_bool(r, "ok", 1);
            resp_str(r, "action", "enqueued");
            resp_str(r, "text", text);
        } else {
            resp_str(r, "error", "queue full");
            return -1;
        }
        return 0;
    }

    /* ai (no args) — status */
    resp_bool(r, "enabled", g_enabled);
    resp_str(r, "llm_url", g_llm_url);
    resp_str(r, "model", g_model);
    resp_str(r, "trigger",
        g_trigger == AI_TRIGGER_WAKE_PHRASE ? "wake_phrase" :
        g_trigger == AI_TRIGGER_DTMF ? "dtmf" : "always");
    resp_str(r, "wake_phrase", g_wake_phrase);
    resp_int(r, "tool_count", g_tool_count);
    resp_int(r, "max_tool_rounds", g_max_tool_rounds);
    resp_int64(r, "total_requests", (int64_t)g_total_requests);
    resp_int64(r, "total_success",  (int64_t)g_total_success);
    resp_int64(r, "total_failures", (int64_t)g_total_failures);
    resp_int(r, "consec_failures", g_consec_failures);
    resp_bool(r, "circuit_open", g_disabled_until != 0 && time(NULL) < g_disabled_until);

    pthread_mutex_lock(&g_convs_mutex);
    int active_convs = 0;
    for (int i = 0; i < AI_MAX_CONVERSATIONS; i++)
        if (g_convs[i].caller_id != 0 && g_convs[i].count > 0)
            active_convs++;
    pthread_mutex_unlock(&g_convs_mutex);
    resp_int(r, "active_conversations", active_convs);
    resp_int(r, "system_prompt_bytes", g_system_prompt ? (int)strlen(g_system_prompt) : 0);

    return 0;

usage:
    resp_text_raw(r,
        "AI voice assistant\n\n"
        "  ai\n"
        "    Show AI status, LLM endpoint, trigger mode, counters.\n\n"
        "  ai tools\n"
        "    List registered tools with descriptions and admin gating.\n\n"
        "  ai history\n"
        "    Show recent queries and responses.\n\n"
        "  ai ask <text>\n"
        "    Manually trigger the AI with text input (bypasses wake phrase).\n\n"
        "  ai reset\n"
        "    Clear all conversation state.\n\n"
        "Config: [ai] llm_url, llm_model, system_prompt_file,\n"
        "        trigger, wake_phrase, max_tokens, temperature\n");
    resp_str(r, "error", "usage: ai [tools|history|ask <text>|reset|help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "ai",
      .usage = "ai [tools|history|ask <text>|reset]",
      .description = "AI voice assistant status and control",
      .handler = cli_ai,
      .category = "AI",
      .subcommands = "tools,history,ask,reset" },
};

static kerchunk_module_def_t mod_ai = {
    .name             = "mod_ai",
    .version          = "1.0.0",
    .description      = "AI voice assistant (ASR → LLM → TTS) with tool calling",
    .load             = ai_load,
    .configure        = ai_configure,
    .unload           = ai_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_ai);
