/*
 * mod_stats.c — Repeater statistics and metrics
 *
 * Uses kerchunk_rrd for mmap'd round-robin storage:
 *   60 x 1-minute slots, 24 x 1-hour slots, 30 x 1-day slots
 *   + scalar counters + per-user stats
 *
 * Data survives restarts. No explicit save/load — mmap handles it.
 *
 * CLI: stats, stats user <name>, stats reset, stats save
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include "kerchunk_rrd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define LOG_MOD "stats"

static kerchunk_core_t *g_core;
static kerchunk_rrd_t  *g_rrd;
static char g_rrd_path[256] = "/var/lib/kerchunk/stats.rrd";

/* In-progress tracking (not persisted — reconstructed on events) */
static uint64_t g_cor_start_us;
static uint64_t g_ptt_start_us;
static int      g_cor_active;
static int      g_ptt_active;
static int      g_current_user_id;

/* Rolling 5-minute duty cycle (kept in-memory, cheap to lose) */
static uint32_t g_duty_bucket[5];
static int      g_duty_idx;
static time_t   g_duty_time;

/* ── Duty cycle ── */

static void duty_rotate(void)
{
    time_t now = time(NULL);
    int elapsed = (int)(now - g_duty_time);
    if (elapsed >= 60) {
        int advance = elapsed / 60;
        if (advance > 5) advance = 5;
        for (int i = 0; i < advance; i++) {
            g_duty_idx = (g_duty_idx + 1) % 5;
            g_duty_bucket[g_duty_idx] = 0;
        }
        g_duty_time = now - (elapsed % 60);
    }
}

static void duty_add_ms(uint32_t ms)
{
    duty_rotate();
    g_duty_bucket[g_duty_idx] += ms;
}

static float duty_pct(void)
{
    duty_rotate();
    uint64_t sum = 0;
    for (int i = 0; i < 5; i++) sum += g_duty_bucket[i];
    if (g_cor_active) {
        uint64_t now_us = (uint64_t)time(NULL) * 1000000ULL;
        if (now_us > g_cor_start_us) sum += (now_us - g_cor_start_us) / 1000;
    }
    if (g_ptt_active) {
        uint64_t now_us = (uint64_t)time(NULL) * 1000000ULL;
        if (now_us > g_ptt_start_us) sum += (now_us - g_ptt_start_us) / 1000;
    }
    if (sum > 300000) sum = 300000;
    return (float)sum / 300000.0f * 100.0f;
}

/* ── Formatters ── */

static int fmt_duration(char *buf, size_t max, uint64_t ms)
{
    uint64_t s = ms / 1000;
    if (s >= 3600) return snprintf(buf, max, "%lluh %02llum %02llus",
        (unsigned long long)(s/3600), (unsigned long long)((s%3600)/60), (unsigned long long)(s%60));
    if (s >= 60) return snprintf(buf, max, "%llum %02llus",
        (unsigned long long)(s/60), (unsigned long long)(s%60));
    if (s > 0) return snprintf(buf, max, "%llu.%llus",
        (unsigned long long)s, (unsigned long long)((ms%1000)/100));
    if (ms > 0) return snprintf(buf, max, "%llums", (unsigned long long)ms);
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

/* ── Event handlers ── */

static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_cor_active = 1;
    g_cor_start_us = evt->timestamp_us;
    g_current_user_id = 0;
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_cor_active) return;
    g_cor_active = 0;

    uint32_t dur_ms = (uint32_t)((evt->timestamp_us - g_cor_start_us) / 1000);
    duty_add_ms(dur_ms);

    /* Record into RRD — handles minute slot, counters, user, kerchunk */
    kerchunk_rrd_record_rx(g_rrd, dur_ms, g_current_user_id);

    /* If user not yet in RRD, create entry */
    if (g_current_user_id > 0) {
        const rrd_file_t *d = kerchunk_rrd_data(g_rrd);
        int found = 0;
        for (uint32_t i = 0; i < d->user_count; i++) {
            if (d->users[i].user_id == g_current_user_id) { found = 1; break; }
        }
        if (!found) {
            const kerchunk_user_t *db = g_core->user_lookup_by_id(g_current_user_id);
            rrd_user_t *u = kerchunk_rrd_user(g_rrd, g_current_user_id,
                db ? db->name : NULL);
            if (u) {
                u->tx_count = 1;
                u->tx_time_ms = dur_ms;
                u->last_heard = time(NULL);
                u->longest_ms = dur_ms;
            }
        }
    }
}

static void on_ptt_assert(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_ptt_active) {
        g_ptt_active = 1;
        g_ptt_start_us = evt->timestamp_us;
    }
}

static void on_ptt_drop(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_ptt_active) return;
    g_ptt_active = 0;
    uint32_t dur_ms = (uint32_t)((evt->timestamp_us - g_ptt_start_us) / 1000);
    duty_add_ms(dur_ms);
}

static void on_caller_identified(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    g_current_user_id = evt->caller.user_id;
}

static void on_timeout(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    kerchunk_rrd_inc(g_rrd, "tot");
}

static void on_queue_complete(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    /* Record TX into current minute slot */
    kerchunk_rrd_record_tx(g_rrd, 0);
}

static void on_recording_saved(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (evt->recording.direction && strcmp(evt->recording.direction, "RX") == 0)
        kerchunk_rrd_inc(g_rrd, "cdr");
}

static void on_announcement(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    const char *src = evt->announcement.source;
    if (!src) return;

    if (strcmp(src, "cwid") == 0) kerchunk_rrd_inc(g_rrd, "cwid");
    else if (strcmp(src, "pocsag") == 0 || strcmp(src, "flex") == 0 ||
             strcmp(src, "aprs") == 0) kerchunk_rrd_inc(g_rrd, "page");
    else if (strcmp(src, "weather") == 0) kerchunk_rrd_inc(g_rrd, "weather");
    else if (strcmp(src, "nws") == 0) kerchunk_rrd_inc(g_rrd, "nws");
    else if (strcmp(src, "freeswitch") == 0) kerchunk_rrd_inc(g_rrd, "phone");
}

static void on_dtmf_end(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    kerchunk_rrd_inc(g_rrd, "dtmf");
}

static void on_tick(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    kerchunk_rrd_tick(g_rrd);
}

static void on_shutdown(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    kerchunk_rrd_sync(g_rrd);
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
    core->subscribe(KERCHEVT_ANNOUNCEMENT,      on_announcement, NULL);
    core->subscribe(KERCHEVT_DTMF_END,          on_dtmf_end, NULL);
    core->subscribe(KERCHEVT_TICK,              on_tick, NULL);
    core->subscribe(KERCHEVT_SHUTDOWN,          on_shutdown, NULL);
    return 0;
}

static int stats_configure(const kerchunk_config_t *cfg)
{
    const char *v = kerchunk_config_get(cfg, "stats", "rrd_file");
    if (v) snprintf(g_rrd_path, sizeof(g_rrd_path), "%s", v);

    /* Legacy compat: if persist_file is set but rrd_file isn't, use a .rrd alongside it */
    if (!v) {
        v = kerchunk_config_get(cfg, "stats", "persist_file");
        if (v) {
            snprintf(g_rrd_path, sizeof(g_rrd_path), "%s", v);
            /* Replace .dat extension with .rrd */
            char *dot = strrchr(g_rrd_path, '.');
            if (dot) snprintf(dot, sizeof(g_rrd_path) - (size_t)(dot - g_rrd_path), ".rrd");
        }
    }

    if (g_rrd) kerchunk_rrd_close(g_rrd);
    g_rrd = kerchunk_rrd_open(g_rrd_path);

    if (!g_rrd) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to open RRD: %s", g_rrd_path);
        return -1;
    }

    memset(g_duty_bucket, 0, sizeof(g_duty_bucket));
    g_duty_idx = 0;
    g_duty_time = time(NULL);

    const rrd_counters_t *c = &kerchunk_rrd_data(g_rrd)->counters;
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "RRD opened: %s (rx=%u tx=%u users=%u restarts=%u)",
                g_rrd_path, c->rx_count, c->tx_count,
                kerchunk_rrd_data(g_rrd)->user_count, c->restarts);
    return 0;
}

static void stats_unload(void)
{
    g_core->unsubscribe(KERCHEVT_COR_ASSERT,       on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP,          on_cor_drop);
    g_core->unsubscribe(KERCHEVT_PTT_ASSERT,        on_ptt_assert);
    g_core->unsubscribe(KERCHEVT_PTT_DROP,          on_ptt_drop);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_TIMEOUT,           on_timeout);
    g_core->unsubscribe(KERCHEVT_QUEUE_COMPLETE,    on_queue_complete);
    g_core->unsubscribe(KERCHEVT_RECORDING_SAVED,   on_recording_saved);
    g_core->unsubscribe(KERCHEVT_ANNOUNCEMENT,      on_announcement);
    g_core->unsubscribe(KERCHEVT_DTMF_END,          on_dtmf_end);
    g_core->unsubscribe(KERCHEVT_TICK,              on_tick);
    g_core->unsubscribe(KERCHEVT_SHUTDOWN,          on_shutdown);

    if (g_rrd) { kerchunk_rrd_close(g_rrd); g_rrd = NULL; }
}

/* ── CLI ── */

static int cli_stats(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;
    if (!g_rrd) { resp_str(r, "error", "RRD not open"); return -1; }

    const rrd_file_t *d = kerchunk_rrd_data(g_rrd);
    const rrd_counters_t *c = &d->counters;

    /* stats reset */
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        kerchunk_rrd_reset(g_rrd);
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "reset");
        return 0;
    }

    /* stats save (force sync) */
    if (argc >= 2 && strcmp(argv[1], "save") == 0) {
        kerchunk_rrd_sync(g_rrd);
        resp_bool(r, "ok", 1);
        resp_str(r, "file", g_rrd_path);
        return 0;
    }

    /* stats user <name> */
    if (argc >= 3 && strcmp(argv[1], "user") == 0) {
        for (uint32_t i = 0; i < d->user_count; i++) {
            if (strcasecmp(d->users[i].name, argv[2]) == 0) {
                const rrd_user_t *u = &d->users[i];
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

    /* ── Full dashboard ── */

    time_t now = time(NULL);
    uint64_t uptime_ms = (uint64_t)difftime(now, c->start_time) * 1000;
    if (uptime_ms == 0) uptime_ms = 1;
    uint64_t total_ms = c->total_uptime_ms + uptime_ms;
    float duty = duty_pct();
    uint32_t avg_rx = c->rx_count > 0 ? (uint32_t)(c->rx_time_ms / c->rx_count) : 0;

    /* Peak hour */
    uint32_t peak_val = 0; int peak_hour = 0;
    for (int i = 0; i < RRD_HOURS; i++) {
        uint32_t v = d->hours[i].rx_count + d->hours[i].tx_count;
        if (v > peak_val) { peak_val = v; peak_hour = i; }
    }

    /* JSON: top-level */
    { char f[128]; snprintf(f, sizeof(f),
        "\"uptime_ms\":%llu,\"total_uptime_ms\":%llu,\"restarts\":%u,",
        (unsigned long long)uptime_ms, (unsigned long long)total_ms, c->restarts);
      resp_json_raw(r, f); }

    /* JSON: channel */
    { char f[768]; snprintf(f, sizeof(f),
        "\"channel\":{"
        "\"rx_count\":%u,\"rx_time_ms\":%llu,"
        "\"tx_count\":%u,\"tx_time_ms\":%llu,"
        "\"duty_pct\":%.1f,\"avg_rx_ms\":%u,"
        "\"longest_rx_ms\":%u,\"shortest_rx_ms\":%u,"
        "\"peak_hour\":%d,\"kerchunk_count\":%u,"
        "\"tot_events\":%u,\"emergency_count\":%u,\"access_denied\":%u,"
        "\"dtmf_commands\":%u,\"cwid_count\":%u,\"pages_sent\":%u,"
        "\"weather_count\":%u,\"nws_alerts\":%u,\"phone_calls\":%u}",
        c->rx_count, (unsigned long long)c->rx_time_ms,
        c->tx_count, (unsigned long long)c->tx_time_ms,
        duty, avg_rx, c->longest_rx_ms, c->shortest_rx_ms,
        peak_hour, c->kerchunk_count,
        c->tot_events, c->emergency_count, c->access_denied,
        c->dtmf_commands, c->cwid_count, c->pages_sent,
        c->weather_count, c->nws_alerts, c->phone_calls);
      resp_json_raw(r, f); }

    /* JSON: minutely (oldest first, labels = real minute of hour) */
    { time_t now = time(NULL);
      struct tm tm_now;
      gmtime_r(&now, &tm_now);
      int cur_min = tm_now.tm_min;
      int cur_hour = tm_now.tm_hour;

      resp_json_raw(r, ",\"minutely\":[");
      for (int i = 0; i < RRD_MINUTES; i++) {
          int idx = (d->minute_idx + 1 + i) % RRD_MINUTES;
          int real_min = (cur_min + 1 + i) % RRD_MINUTES;
          if (i > 0) resp_json_raw(r, ",");
          char f[64]; snprintf(f, sizeof(f), "{\"min\":%d,\"rx\":%u,\"tx\":%u}",
              real_min, d->minutes[idx].rx_count, d->minutes[idx].tx_count);
          resp_json_raw(r, f);
      }
      resp_json_raw(r, "]");

      /* JSON: hourly (oldest first, labels = real hour of day) */
      resp_json_raw(r, ",\"hourly\":[");
      for (int i = 0; i < RRD_HOURS; i++) {
          int idx = (d->hour_idx + 1 + i) % RRD_HOURS;
          int real_hour = (cur_hour + 1 + i) % RRD_HOURS;
          if (i > 0) resp_json_raw(r, ",");
          char f[64]; snprintf(f, sizeof(f), "{\"hour\":%d,\"rx\":%u,\"tx\":%u}",
              real_hour, d->hours[idx].rx_count, d->hours[idx].tx_count);
          resp_json_raw(r, f);
      }
      resp_json_raw(r, "]");
    }

    /* JSON: daily (oldest first) */
    resp_json_raw(r, ",\"daily\":[");
    for (int i = 0; i < RRD_DAYS; i++) {
        int idx = (d->day_idx + 1 + i) % RRD_DAYS;
        if (i > 0) resp_json_raw(r, ",");
        char f[64]; snprintf(f, sizeof(f), "{\"day\":%d,\"rx\":%u,\"tx\":%u}",
            i, d->days[idx].rx_count, d->days[idx].tx_count);
        resp_json_raw(r, f);
    }
    resp_json_raw(r, "]");

    /* JSON: users */
    resp_json_raw(r, ",\"users\":[");
    for (uint32_t i = 0; i < d->user_count; i++) {
        const rrd_user_t *u = &d->users[i];
        if (i > 0) resp_json_raw(r, ",");
        char f[256]; snprintf(f, sizeof(f),
            "{\"id\":%d,\"name\":\"%s\",\"tx_count\":%u,"
            "\"tx_time_ms\":%llu,\"last_heard\":%lld,\"longest_ms\":%u}",
            u->user_id, u->name, u->tx_count,
            (unsigned long long)u->tx_time_ms,
            (long long)u->last_heard, u->longest_ms);
        resp_json_raw(r, f);
    }
    resp_json_raw(r, "]");

    /* JSON: system */
    { char f[128]; snprintf(f, sizeof(f),
        ",\"system\":{\"queue_items\":%u,\"cdr_records\":%u}",
        c->queue_items, c->cdr_records);
      resp_json_raw(r, f); }
    r->jfirst = 0;

    /* ── Text dashboard ── */
    char up_s[32], rx_s[32], tx_s[32], idle_s[32];
    fmt_duration(up_s, sizeof(up_s), uptime_ms);
    fmt_duration(rx_s, sizeof(rx_s), c->rx_time_ms);
    fmt_duration(tx_s, sizeof(tx_s), c->tx_time_ms);
    uint64_t busy = c->rx_time_ms + c->tx_time_ms;
    if (busy > uptime_ms) busy = uptime_ms;
    fmt_duration(idle_s, sizeof(idle_s), uptime_ms - busy);

    { char l[256]; snprintf(l, sizeof(l),
        "Repeater Statistics\n===================\nUptime: %s", up_s);
      resp_text_raw(r, l); }
    if (c->restarts > 0) {
        char ts[32], l[128];
        fmt_duration(ts, sizeof(ts), total_ms);
        snprintf(l, sizeof(l), " (total %s, %u restart%s)",
            ts, c->restarts, c->restarts == 1 ? "" : "s");
        resp_text_raw(r, l);
    }
    resp_text_raw(r, "\n\n");
    { char l[256]; snprintf(l, sizeof(l),
        "Channel:\n"
        "  Inbound (RX):   %u transmissions, %s\n"
        "  Outbound (TX):  %u announcements, %s (duty %.1f%%)\n"
        "  Idle:           %s\n",
        c->rx_count, rx_s, c->tx_count, tx_s, duty, idle_s);
      resp_text_raw(r, l); }
    if (c->rx_count > 0) {
        char a[32], lo[32], sh[32], l[160];
        fmt_duration(a, sizeof(a), avg_rx);
        fmt_duration(lo, sizeof(lo), c->longest_rx_ms);
        fmt_duration(sh, sizeof(sh), c->shortest_rx_ms);
        snprintf(l, sizeof(l), "  Avg inbound:    %s  (shortest %s, longest %s)\n", a, sh, lo);
        resp_text_raw(r, l);
    }
    { char l[256]; snprintf(l, sizeof(l),
        "  Kerchunks:      %u\n  DTMF commands:  %u\n  CW IDs:         %u\n  Pages sent:     %u\n",
        c->kerchunk_count, c->dtmf_commands, c->cwid_count, c->pages_sent);
      resp_text_raw(r, l); }
    if (c->tot_events || c->emergency_count) {
        char l[96]; snprintf(l, sizeof(l), "  TOT events:     %u  Emergencies: %u\n",
            c->tot_events, c->emergency_count);
        resp_text_raw(r, l);
    }

    /* Text: hourly histogram */
    { uint32_t mx = 0; int has = 0;
      for (int i = 0; i < RRD_HOURS; i++) {
          uint32_t t = d->hours[i].rx_count + d->hours[i].tx_count;
          if (t > mx) mx = t;
          if (t) has = 1;
      }
      if (has) {
          resp_text_raw(r, "\n24h Activity:\n");
          for (int i = 0; i < RRD_HOURS; i++) {
              int idx = (d->hour_idx + 1 + i) % RRD_HOURS;
              uint32_t rx = d->hours[idx].rx_count, tx = d->hours[idx].tx_count;
              uint32_t t = rx + tx;
              int bars = mx > 0 ? (int)((t * 10 + mx - 1) / mx) : 0;
              if (!t) bars = 0;
              char bar[11]; int b;
              for (b = 0; b < bars && b < 10; b++) bar[b] = '#';
              for (; b < 10; b++) bar[b] = '.';
              bar[10] = '\0';
              char l[80]; snprintf(l, sizeof(l), "  %02d %s %3u rx %3u tx%s\n",
                  i, bar, rx, tx, i == RRD_HOURS - 1 ? "  <-- now" : "");
              resp_text_raw(r, l);
          }
      }
    }

    /* Text: top users */
    if (d->user_count > 0) {
        resp_text_raw(r, "\nTop Users:\n");
        int idx[RRD_MAX_USERS];
        for (uint32_t i = 0; i < d->user_count; i++) idx[i] = (int)i;
        for (uint32_t i = 0; i < d->user_count - 1; i++)
            for (uint32_t j = i + 1; j < d->user_count; j++)
                if (d->users[idx[j]].tx_count > d->users[idx[i]].tx_count)
                    { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
        uint32_t show = d->user_count < 10 ? d->user_count : 10;
        for (uint32_t i = 0; i < show; i++) {
            const rrd_user_t *u = &d->users[idx[i]];
            char dur[32], last[32], l[128];
            fmt_duration(dur, sizeof(dur), u->tx_time_ms);
            fmt_ago(last, sizeof(last), (time_t)u->last_heard);
            snprintf(l, sizeof(l), "  %-14s  %4u TX  %10s  %s\n",
                u->name, u->tx_count, dur, last);
            resp_text_raw(r, l);
        }
    }

    { char l[512]; snprintf(l, sizeof(l),
        "\nSystem:\n  Queue played:   %u\n  CDR records:    %u\n  Storage:        %s (mmap'd RRD)\n",
        c->queue_items, c->cdr_records, g_rrd_path);
      resp_text_raw(r, l); }

    return 0;

usage:
    resp_text_raw(r, "Repeater statistics and metrics\n\n"
        "  stats\n"
        "    Full dashboard: uptime, channel, histograms, top users.\n\n"
        "  stats user <name>\n"
        "    Per-user statistics.\n\n"
        "  stats reset\n"
        "    Reset all data (preserves restart count).\n\n"
        "  stats save\n"
        "    Force sync RRD to disk.\n\n"
        "Storage: mmap'd round-robin database (survives restarts).\n"
        "Config: [stats] rrd_file (default /var/lib/kerchunk/stats.rrd)\n");
    resp_str(r, "error", "usage: stats [user <name>|reset|save]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "stats", .usage = "stats [user <name>|reset|save]",
      .description = "Repeater statistics", .handler = cli_stats, .category = "Control" },
};

static kerchunk_module_def_t mod_stats = {
    .name             = "mod_stats",
    .version          = "3.0.0",
    .description      = "Repeater statistics (mmap'd RRD)",
    .load             = stats_load,
    .configure        = stats_configure,
    .unload           = stats_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_stats);
