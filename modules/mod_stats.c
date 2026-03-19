/*
 * mod_stats.c — Repeater statistics and metrics
 *
 * Tracks channel, per-user, and system metrics via event subscriptions.
 * Rolling 24-hour histogram by hour. Persists to disk on shutdown,
 * restores on startup.
 *
 * CLI: stats, stats user <name>, stats reset
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define LOG_MOD "stats"

static kerchunk_core_t *g_core;

/* Config */
static int  g_persist       = 0;
static char g_persist_file[256] = "stats.dat";

/* ── Channel stats ── */

static struct {
    uint64_t rx_time_ms;
    uint64_t tx_time_ms;
    uint32_t rx_count;
    uint32_t tx_count;         /* Queue playback cycles */
    uint32_t tot_events;
    uint32_t emergency_count;
    uint32_t longest_rx_ms;
    uint32_t shortest_rx_ms;
    uint32_t access_denied;

    /* Rolling 24h histogram */
    uint32_t hourly_rx_count[24];
    uint32_t hourly_rx_ms[24];
    uint32_t hourly_tx_count[24];
    int      current_hour;

    /* In-progress */
    uint64_t cor_start_us;
    uint64_t ptt_start_us;
    int      cor_active;
    int      ptt_active;
    int      current_user_id;
} g_ch;

/* ── Per-user stats ── */

#define MAX_USER_STATS 64

typedef struct {
    int      user_id;
    char     name[32];
    uint32_t tx_count;
    uint64_t tx_time_ms;
    time_t   last_heard;
    uint32_t longest_ms;
} user_stats_t;

static user_stats_t g_users[MAX_USER_STATS];
static int          g_user_count;

/* ── System stats ── */

static struct {
    time_t   start_time;       /* This session's start */
    uint64_t total_uptime_ms;  /* Accumulated across all sessions */
    uint32_t cdr_records;
    uint32_t queue_items;
    uint32_t restarts;
} g_sys;

/* ── Helpers ── */

static user_stats_t *find_or_create_user(int user_id)
{
    for (int i = 0; i < g_user_count; i++) {
        if (g_users[i].user_id == user_id)
            return &g_users[i];
    }
    if (g_user_count >= MAX_USER_STATS)
        return NULL;

    user_stats_t *u = &g_users[g_user_count++];
    memset(u, 0, sizeof(*u));
    u->user_id = user_id;

    const kerchunk_user_t *db = g_core->user_lookup_by_id(user_id);
    if (db)
        snprintf(u->name, sizeof(u->name), "%s", db->name);
    else
        snprintf(u->name, sizeof(u->name), "user_%d", user_id);

    return u;
}

static void rotate_hour(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int hour = t->tm_hour;

    if (hour != g_ch.current_hour) {
        g_ch.hourly_rx_count[hour] = 0;
        g_ch.hourly_rx_ms[hour] = 0;
        g_ch.hourly_tx_count[hour] = 0;
        g_ch.current_hour = hour;
    }
}

static int fmt_duration(char *buf, size_t max, uint64_t ms)
{
    uint64_t secs = ms / 1000;
    if (secs >= 3600)
        return snprintf(buf, max, "%lluh %02llum %02llus",
                 (unsigned long long)(secs / 3600),
                 (unsigned long long)((secs % 3600) / 60),
                 (unsigned long long)(secs % 60));
    else if (secs >= 60)
        return snprintf(buf, max, "%llum %02llus",
                 (unsigned long long)(secs / 60),
                 (unsigned long long)(secs % 60));
    else if (secs > 0)
        return snprintf(buf, max, "%llu.%llus",
                 (unsigned long long)secs,
                 (unsigned long long)((ms % 1000) / 100));
    else if (ms > 0)
        return snprintf(buf, max, "%llums", (unsigned long long)ms);
    else
        return snprintf(buf, max, "0s");
}

static int fmt_ago(char *buf, size_t max, time_t then)
{
    if (then == 0) return snprintf(buf, max, "never");
    int ago = (int)difftime(time(NULL), then);
    if (ago < 60)   return snprintf(buf, max, "%ds ago", ago);
    if (ago < 3600) return snprintf(buf, max, "%dm ago", ago / 60);
    return snprintf(buf, max, "%dh %dm ago", ago / 3600, (ago % 3600) / 60);
}

/* ── Persistence ── */

#define STATS_MAGIC 0x52505453  /* "RPTS" */
#define STATS_VERSION 2

typedef struct {
    uint32_t magic;
    uint32_t version;
    /* Channel */
    uint64_t rx_time_ms;
    uint64_t tx_time_ms;
    uint32_t rx_count;
    uint32_t tx_count;
    uint32_t tot_events;
    uint32_t emergency_count;
    uint32_t longest_rx_ms;
    uint32_t shortest_rx_ms;
    uint32_t access_denied;
    /* Hourly histogram (24 slots) */
    uint32_t hourly_rx_count[24];
    uint32_t hourly_rx_ms[24];
    uint32_t hourly_tx_count[24];
    int32_t  saved_hour;
    /* System */
    uint64_t total_uptime_ms;  /* Accumulated across all sessions */
    uint32_t cdr_records;
    uint32_t queue_items;
    uint32_t restarts;
    /* Users */
    uint32_t user_count;
    /* Followed by user_count * user_stats_t */
} stats_file_header_t;

static void save_stats(void)
{
    if (!g_persist) return;

    FILE *fp = fopen(g_persist_file, "wb");
    if (!fp) {
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "cannot save stats: %s",
                    g_persist_file);
        return;
    }

    /* Accumulate this session's uptime into total */
    uint64_t session_ms = (uint64_t)difftime(time(NULL), g_sys.start_time) * 1000;

    stats_file_header_t hdr = {
        .magic           = STATS_MAGIC,
        .version         = STATS_VERSION,
        .rx_time_ms      = g_ch.rx_time_ms,
        .tx_time_ms      = g_ch.tx_time_ms,
        .rx_count        = g_ch.rx_count,
        .tx_count        = g_ch.tx_count,
        .tot_events      = g_ch.tot_events,
        .emergency_count = g_ch.emergency_count,
        .longest_rx_ms   = g_ch.longest_rx_ms,
        .shortest_rx_ms  = g_ch.shortest_rx_ms,
        .access_denied   = g_ch.access_denied,
        .saved_hour      = g_ch.current_hour,
        .total_uptime_ms = g_sys.total_uptime_ms + session_ms,
        .cdr_records     = g_sys.cdr_records,
        .queue_items     = g_sys.queue_items,
        .restarts        = g_sys.restarts,
        .user_count      = (uint32_t)g_user_count,
    };
    memcpy(hdr.hourly_rx_count, g_ch.hourly_rx_count, sizeof(hdr.hourly_rx_count));
    memcpy(hdr.hourly_rx_ms, g_ch.hourly_rx_ms, sizeof(hdr.hourly_rx_ms));
    memcpy(hdr.hourly_tx_count, g_ch.hourly_tx_count, sizeof(hdr.hourly_tx_count));

    fwrite(&hdr, sizeof(hdr), 1, fp);
    fwrite(g_users, sizeof(user_stats_t), (size_t)g_user_count, fp);
    fclose(fp);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "stats saved to %s",
                g_persist_file);
}

static void load_stats(void)
{
    if (!g_persist) return;

    FILE *fp = fopen(g_persist_file, "rb");
    if (!fp) return;

    stats_file_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 ||
        hdr.magic != STATS_MAGIC || hdr.version != STATS_VERSION) {
        fclose(fp);
        g_core->log(KERCHUNK_LOG_WARN, LOG_MOD,
                    "invalid stats file, starting fresh");
        return;
    }

    g_ch.rx_time_ms      = hdr.rx_time_ms;
    g_ch.tx_time_ms      = hdr.tx_time_ms;
    g_ch.rx_count        = hdr.rx_count;
    g_ch.tx_count        = hdr.tx_count;
    g_ch.tot_events      = hdr.tot_events;
    g_ch.emergency_count = hdr.emergency_count;
    g_ch.longest_rx_ms   = hdr.longest_rx_ms;
    g_ch.shortest_rx_ms  = hdr.shortest_rx_ms;
    g_ch.access_denied   = hdr.access_denied;
    g_ch.current_hour    = hdr.saved_hour;
    memcpy(g_ch.hourly_rx_count, hdr.hourly_rx_count, sizeof(g_ch.hourly_rx_count));
    memcpy(g_ch.hourly_rx_ms, hdr.hourly_rx_ms, sizeof(g_ch.hourly_rx_ms));
    memcpy(g_ch.hourly_tx_count, hdr.hourly_tx_count, sizeof(g_ch.hourly_tx_count));
    g_sys.total_uptime_ms = hdr.total_uptime_ms;
    g_sys.cdr_records    = hdr.cdr_records;
    g_sys.queue_items    = hdr.queue_items;
    g_sys.restarts       = hdr.restarts + 1;

    int n = (int)hdr.user_count;
    if (n > MAX_USER_STATS) n = MAX_USER_STATS;
    if (fread(g_users, sizeof(user_stats_t), (size_t)n, fp) == (size_t)n)
        g_user_count = n;

    fclose(fp);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "stats restored from %s (restart #%u)",
                g_persist_file, g_sys.restarts);
}

/* ── Event handlers ── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_ch.cor_active = 1;
    g_ch.cor_start_us = evt->timestamp_us;
    g_ch.current_user_id = 0;
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_ch.cor_active) return;
    g_ch.cor_active = 0;

    uint64_t dur_ms = (evt->timestamp_us - g_ch.cor_start_us) / 1000;

    g_ch.rx_time_ms += dur_ms;
    g_ch.rx_count++;

    if (dur_ms > g_ch.longest_rx_ms)
        g_ch.longest_rx_ms = (uint32_t)dur_ms;
    if (g_ch.shortest_rx_ms == 0 || dur_ms < g_ch.shortest_rx_ms)
        g_ch.shortest_rx_ms = (uint32_t)dur_ms;

    rotate_hour();
    g_ch.hourly_rx_count[g_ch.current_hour]++;
    g_ch.hourly_rx_ms[g_ch.current_hour] += (uint32_t)dur_ms;

    if (g_ch.current_user_id > 0) {
        user_stats_t *u = find_or_create_user(g_ch.current_user_id);
        if (u) {
            u->tx_count++;
            u->tx_time_ms += dur_ms;
            u->last_heard = time(NULL);
            if (dur_ms > u->longest_ms)
                u->longest_ms = (uint32_t)dur_ms;
        }
    }
}

static void on_ptt_assert(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_ch.ptt_active) {
        g_ch.ptt_active = 1;
        g_ch.ptt_start_us = evt->timestamp_us;
    }
}

static void on_ptt_drop(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_ch.ptt_active) return;
    g_ch.ptt_active = 0;

    uint64_t dur_ms = (evt->timestamp_us - g_ch.ptt_start_us) / 1000;
    g_ch.tx_time_ms += dur_ms;
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_ch.current_user_id = evt->caller.user_id;
}

static void on_timeout(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_ch.tot_events++;
}

static void on_queue_complete(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    g_sys.queue_items++;
    g_ch.tx_count++;
    rotate_hour();
    g_ch.hourly_tx_count[g_ch.current_hour]++;
}

static void on_recording_saved(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (evt->recording.direction &&
        strcmp(evt->recording.direction, "RX") == 0)
        g_sys.cdr_records++;
}

static void on_shutdown(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    save_stats();
}

/* ── Module lifecycle ── */

static int stats_load(kerchunk_core_t *core)
{
    g_core = core;
    core->subscribe(KERCHEVT_COR_ASSERT,       on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP,          on_cor_drop, NULL);
    core->subscribe(KERCHEVT_PTT_ASSERT,        on_ptt_assert, NULL);
    core->subscribe(KERCHEVT_PTT_DROP,          on_ptt_drop, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    core->subscribe(KERCHEVT_TIMEOUT,           on_timeout, NULL);
    core->subscribe(KERCHEVT_QUEUE_COMPLETE,    on_queue_complete, NULL);
    core->subscribe(KERCHEVT_RECORDING_SAVED,   on_recording_saved, NULL);
    core->subscribe(KERCHEVT_SHUTDOWN,          on_shutdown, NULL);
    return 0;
}

static int stats_configure(const kerchunk_config_t *cfg)
{
    const char *v;

    v = kerchunk_config_get(cfg, "stats", "persist");
    g_persist = (v && strcmp(v, "on") == 0);

    v = kerchunk_config_get(cfg, "stats", "persist_file");
    if (v) snprintf(g_persist_file, sizeof(g_persist_file), "%s", v);

    /* Initialize fresh */
    memset(&g_ch, 0, sizeof(g_ch));
    memset(&g_users, 0, sizeof(g_users));
    g_user_count = 0;
    memset(&g_sys, 0, sizeof(g_sys));
    g_sys.start_time = time(NULL);
    g_sys.total_uptime_ms = 0;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    g_ch.current_hour = t->tm_hour;

    /* Restore from disk if persistence is on */
    load_stats();

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "started (persist=%s, rx=%u, tx=%u, users=%d)",
                g_persist ? g_persist_file : "off",
                g_ch.rx_count, g_ch.tx_count, g_user_count);
    return 0;
}

static void stats_unload(void)
{
    save_stats();

    g_core->unsubscribe(KERCHEVT_COR_ASSERT,       on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,          on_cor_drop);
    g_core->unsubscribe(KERCHEVT_PTT_ASSERT,        on_ptt_assert);
    g_core->unsubscribe(KERCHEVT_PTT_DROP,          on_ptt_drop);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_TIMEOUT,           on_timeout);
    g_core->unsubscribe(KERCHEVT_QUEUE_COMPLETE,    on_queue_complete);
    g_core->unsubscribe(KERCHEVT_RECORDING_SAVED,   on_recording_saved);
    g_core->unsubscribe(KERCHEVT_SHUTDOWN,          on_shutdown);
}

/* ── CLI ── */

static int cli_stats(int argc, const char **argv, kerchunk_resp_t *r)
{
    /* stats reset */
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        memset(&g_ch, 0, sizeof(g_ch));
        memset(&g_users, 0, sizeof(g_users));
        g_user_count = 0;
        uint32_t restarts = g_sys.restarts;
        memset(&g_sys, 0, sizeof(g_sys));
        g_sys.start_time = time(NULL);
        g_sys.total_uptime_ms = 0;
        g_sys.restarts = restarts;
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        g_ch.current_hour = t->tm_hour;
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "reset");
        return 0;
    }

    /* stats save */
    if (argc >= 2 && strcmp(argv[1], "save") == 0) {
        g_persist = 1;
        save_stats();
        resp_bool(r, "ok", 1);
        resp_str(r, "file", g_persist_file);
        return 0;
    }

    /* stats user <name> */
    if (argc >= 3 && strcmp(argv[1], "user") == 0) {
        for (int i = 0; i < g_user_count; i++) {
            if (strcasecmp(g_users[i].name, argv[2]) == 0) {
                user_stats_t *u = &g_users[i];
                resp_str(r, "name", u->name);
                resp_int(r, "user_id", u->user_id);
                resp_int(r, "tx_count", (int)u->tx_count);
                resp_int64(r, "tx_time_ms", (int64_t)u->tx_time_ms);
                resp_int(r, "longest_ms", (int)u->longest_ms);
                resp_int64(r, "last_heard", (int64_t)u->last_heard);
                return 0;
            }
        }
        resp_str(r, "error", "User not found");
        return 0;
    }

    /* ── Full stats dashboard ── */

    time_t now = time(NULL);
    uint64_t uptime_ms = (uint64_t)difftime(now, g_sys.start_time) * 1000;
    if (uptime_ms == 0) uptime_ms = 1;
    uint64_t total_ms = g_sys.total_uptime_ms + uptime_ms;
    float duty = (float)g_ch.tx_time_ms / (float)uptime_ms * 100.0f;

    /* JSON-only top-level fields (text uses custom dashboard below) */
    {
        char jfrag[128];
        snprintf(jfrag, sizeof(jfrag),
            "\"uptime_ms\":%llu,\"total_uptime_ms\":%llu,\"restarts\":%u,",
            (unsigned long long)uptime_ms,
            (unsigned long long)total_ms,
            g_sys.restarts);
        resp_json_raw(r, jfrag);
    }
    char ch_frag[512];
    snprintf(ch_frag, sizeof(ch_frag),
        "\"channel\":{"
        "\"rx_count\":%u,\"rx_time_ms\":%llu,"
        "\"tx_count\":%u,\"tx_time_ms\":%llu,"
        "\"duty_pct\":%.1f,"
        "\"longest_rx_ms\":%u,\"shortest_rx_ms\":%u,"
        "\"tot_events\":%u,\"emergency_count\":%u,\"access_denied\":%u}",
        g_ch.rx_count, (unsigned long long)g_ch.rx_time_ms,
        g_ch.tx_count, (unsigned long long)g_ch.tx_time_ms,
        duty,
        g_ch.longest_rx_ms, g_ch.shortest_rx_ms,
        g_ch.tot_events, g_ch.emergency_count, g_ch.access_denied);
    resp_json_raw(r, ch_frag);

    /* JSON: hourly histogram */
    rotate_hour();
    struct tm *tnow = localtime(&now);
    resp_json_raw(r, ",\"hourly\":[");
    for (int i = 0; i < 24; i++) {
        int h = (tnow->tm_hour + 1 + i) % 24;
        if (i > 0) resp_json_raw(r, ",");
        char hfrag[64];
        snprintf(hfrag, sizeof(hfrag),
            "{\"hour\":%d,\"rx\":%u,\"tx\":%u}",
            h, g_ch.hourly_rx_count[h], g_ch.hourly_tx_count[h]);
        resp_json_raw(r, hfrag);
    }
    resp_json_raw(r, "]");

    /* JSON: users array */
    resp_json_raw(r, ",\"users\":[");
    for (int i = 0; i < g_user_count; i++) {
        user_stats_t *u = &g_users[i];
        if (i > 0) resp_json_raw(r, ",");
        char ufrag[256];
        snprintf(ufrag, sizeof(ufrag),
            "{\"id\":%d,\"name\":\"%s\",\"tx_count\":%u,"
            "\"tx_time_ms\":%llu,\"last_heard\":%ld,\"longest_ms\":%u}",
            u->user_id, u->name, u->tx_count,
            (unsigned long long)u->tx_time_ms,
            (long)u->last_heard, u->longest_ms);
        resp_json_raw(r, ufrag);
    }
    resp_json_raw(r, "]");

    /* JSON: system */
    {
        char sfrag[128];
        snprintf(sfrag, sizeof(sfrag),
            ",\"system\":{\"queue_items\":%u,\"cdr_records\":%u}",
            g_sys.queue_items, g_sys.cdr_records);
        resp_json_raw(r, sfrag);
    }
    r->jfirst = 0;

    /* ── Text: human-readable dashboard ── */
    char up_s[32], rx_s[32], tx_s[32], idle_s[32];
    fmt_duration(up_s, sizeof(up_s), uptime_ms);
    fmt_duration(rx_s, sizeof(rx_s), g_ch.rx_time_ms);
    fmt_duration(tx_s, sizeof(tx_s), g_ch.tx_time_ms);
    uint64_t busy_ms = g_ch.rx_time_ms + g_ch.tx_time_ms;
    uint64_t idle_ms = (uptime_ms > busy_ms) ? uptime_ms - busy_ms : 0;
    fmt_duration(idle_s, sizeof(idle_s), idle_ms);

    {
        char line[256];
        snprintf(line, sizeof(line),
            "Repeater Statistics\n===================\nUptime: %s", up_s);
        resp_text_raw(r, line);
    }
    if (g_sys.restarts > 0) {
        char total_s[32], line[128];
        fmt_duration(total_s, sizeof(total_s), total_ms);
        snprintf(line, sizeof(line), " (total %s, %u restart%s)",
                 total_s, g_sys.restarts, g_sys.restarts == 1 ? "" : "s");
        resp_text_raw(r, line);
    }
    resp_text_raw(r, "\n\n");
    {
        char line[256];
        snprintf(line, sizeof(line),
            "Channel:\n"
            "  Inbound (RX):   %u transmissions, %s\n"
            "  Outbound (TX):  %u announcements, %s (duty %.1f%%)\n"
            "  Idle:           %s\n",
            g_ch.rx_count, rx_s, g_ch.tx_count, tx_s, duty, idle_s);
        resp_text_raw(r, line);
    }
    if (g_ch.rx_count > 0) {
        char avg[32], longest[32], shortest[32], line[160];
        fmt_duration(avg, sizeof(avg), g_ch.rx_time_ms / g_ch.rx_count);
        fmt_duration(longest, sizeof(longest), g_ch.longest_rx_ms);
        fmt_duration(shortest, sizeof(shortest), g_ch.shortest_rx_ms);
        snprintf(line, sizeof(line),
            "  Avg inbound:    %s  (shortest %s, longest %s)\n",
            avg, shortest, longest);
        resp_text_raw(r, line);
    }
    if (g_ch.tot_events > 0) {
        char line[64];
        snprintf(line, sizeof(line), "  TOT events:     %u\n", g_ch.tot_events);
        resp_text_raw(r, line);
    }
    /* Text: 24h histogram (only if data exists) */
    {
        uint32_t max_hr = 0;
        int has_data = 0;
        for (int i = 0; i < 24; i++) {
            uint32_t total = g_ch.hourly_rx_count[i] + g_ch.hourly_tx_count[i];
            if (total > max_hr) max_hr = total;
            if (total > 0) has_data = 1;
        }
        if (has_data) {
            resp_text_raw(r, "\n24h Activity:\n");
            for (int i = 0; i < 24; i++) {
                int h = (tnow->tm_hour + 1 + i) % 24;
                uint32_t rx = g_ch.hourly_rx_count[h];
                uint32_t tx = g_ch.hourly_tx_count[h];
                uint32_t total = rx + tx;
                int bars = (max_hr > 0) ? (int)((total * 10 + max_hr - 1) / max_hr) : 0;
                if (total == 0) bars = 0;
                char bar[11];
                int b;
                for (b = 0; b < bars && b < 10; b++) bar[b] = '#';
                for (; b < 10; b++) bar[b] = '.';
                bar[10] = '\0';
                char line[80];
                snprintf(line, sizeof(line), "  %02d %s %3u rx %3u tx%s\n",
                         h, bar, rx, tx,
                         (h == tnow->tm_hour) ? "  <-- now" : "");
                resp_text_raw(r, line);
            }
        }
    }

    /* Text: top users */
    if (g_user_count > 0) {
        resp_text_raw(r, "\nTop Users:\n");
        int idx[MAX_USER_STATS];
        for (int i = 0; i < g_user_count; i++) idx[i] = i;
        for (int i = 0; i < g_user_count - 1; i++) {
            for (int j = i + 1; j < g_user_count; j++) {
                if (g_users[idx[j]].tx_count > g_users[idx[i]].tx_count) {
                    int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
                }
            }
        }
        int show = g_user_count < 10 ? g_user_count : 10;
        for (int i = 0; i < show; i++) {
            user_stats_t *u = &g_users[idx[i]];
            char dur[32], last[32], line[128];
            fmt_duration(dur, sizeof(dur), u->tx_time_ms);
            fmt_ago(last, sizeof(last), u->last_heard);
            snprintf(line, sizeof(line), "  %-14s  %4u TX  %10s  %s\n",
                     u->name, u->tx_count, dur, last);
            resp_text_raw(r, line);
        }
    }

    {
        char line[288];
        snprintf(line, sizeof(line),
            "\nSystem:\n  Queue played:   %u\n  CDR records:    %u\n",
            g_sys.queue_items, g_sys.cdr_records);
        resp_text_raw(r, line);
        if (g_persist) {
            snprintf(line, sizeof(line), "  Persistence:    %s\n",
                     g_persist_file);
            resp_text_raw(r, line);
        }
    }

    return 0;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "stats", "stats [user <name>|reset|save]",
      "Repeater statistics", cli_stats },
};

static kerchunk_module_def_t mod_stats = {
    .name             = "mod_stats",
    .version          = "1.0.0",
    .description      = "Repeater statistics and metrics",
    .load             = stats_load,
    .configure        = stats_configure,
    .unload           = stats_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_stats);
