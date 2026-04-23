/*
 * mod_sysstats.c — Host system metrics (CPU, memory, temperature, network)
 *
 * Samples /proc and /sys every N seconds; CLI `sys` returns a JSON
 * snapshot. Admin UI reaches it at /admin/api/sys through mod_web's
 * generic CLI-to-REST dispatch.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define LOG_MOD "sysstats"
#define MAX_IFACE 32

/* ───────────────────────── Historical RRD ─────────────────────────
 * Continuous gauges (CPU%, mem%, temp, load, net bps) want averaging,
 * not the counter-inc model kerchunk_rrd offers. This is a separate
 * parallel file for sysstats with its own slot shape: one float per
 * metric per slot, plus an epoch timestamp to distinguish "this slot
 * was written for minute T" from "stale data from the previous
 * ring-wrap". Ring indexing is wall-clock driven: minute slot i
 * belongs to epoch minutes where (epoch/60) % 60 == i.
 *
 * 60 minute + 24 hour + 30 day slots = 114 * ~40 B ≈ 4.5 KB on disk.
 */
#define SYS_RRD_MAGIC   0x44525353u   /* 'SSRD' */
#define SYS_RRD_VERSION 1u
#define SYS_RRD_MINS    60
#define SYS_RRD_HOURS   24
#define SYS_RRD_DAYS    30

typedef struct {
    int64_t  t;              /* epoch seconds at slot start; 0 = never written */
    float    cpu_pct;
    float    mem_used_pct;
    float    temp_c;
    float    load1, load5, load15;
    float    net_rx_bps;
    float    net_tx_bps;
} sys_slot_t;

typedef struct {
    uint32_t   magic;
    uint32_t   version;
    int64_t    start_time;
    uint64_t   total_samples;
    uint32_t   _reserved[10];
    sys_slot_t minutes[SYS_RRD_MINS];
    sys_slot_t hours[SYS_RRD_HOURS];
    sys_slot_t days[SYS_RRD_DAYS];
} sys_rrd_t;

/* Running-average accumulators for each horizon. Each 5s sample feeds
 * all three independently so day/hour/minute averages are each a true
 * mean of their raw samples (not mean-of-means). */
typedef struct {
    double   sum_cpu, sum_mem, sum_temp;
    double   sum_l1,  sum_l5,  sum_l15;
    double   sum_rx,  sum_tx;
    uint32_t count;
} sys_acc_t;

static sys_rrd_t *g_rrd;
static int        g_rrd_fd   = -1;
static char       g_rrd_path[256] = "/var/lib/kerchunk/sysstats.rrd";
static sys_acc_t  g_min_acc, g_hour_acc, g_day_acc;
static time_t     g_last_rollover_t;    /* wall-clock of previous sample */
static int        g_history_sched = -1;

static kerchunk_core_t *g_core;

/* Config */
static int  g_enabled      = 1;
static int  g_interval_ms  = 5000;
static char g_iface[MAX_IFACE];
static int  g_sched_id     = -1;

/* Latest sample */
static double   g_cpu_pct;
static long     g_mem_total_kb;
static long     g_mem_avail_kb;
static double   g_temp_c;
static double   g_load1, g_load5, g_load15;
static uint64_t g_net_rx_bps, g_net_tx_bps;
static time_t   g_last_sample_time;
static int      g_have_sample;

/* Delta state */
static uint64_t g_prev_cpu_total;
static uint64_t g_prev_cpu_idle;
static uint64_t g_prev_net_rx;
static uint64_t g_prev_net_tx;
static struct timespec g_prev_net_ts;
static int      g_have_prev_cpu;
static int      g_have_prev_net;

static void read_cpu(void)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return;
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0,
                       iowait = 0, irq = 0, softirq = 0, steal = 0;
    int n = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal);
    fclose(fp);
    if (n < 4) return;

    uint64_t total    = user + nice + sys + idle + iowait + irq + softirq + steal;
    uint64_t idle_all = idle + iowait;

    if (g_have_prev_cpu) {
        uint64_t dtotal = total - g_prev_cpu_total;
        uint64_t didle  = idle_all - g_prev_cpu_idle;
        if (dtotal > 0)
            g_cpu_pct = 100.0 * (double)(dtotal - didle) / (double)dtotal;
    }
    g_prev_cpu_total = total;
    g_prev_cpu_idle  = idle_all;
    g_have_prev_cpu  = 1;
}

static void read_meminfo(void)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) return;
    char line[256];
    long total = 0, avail = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) total = atol(line + 9);
        else if (strncmp(line, "MemAvailable:", 13) == 0) avail = atol(line + 13);
    }
    fclose(fp);
    if (total > 0) { g_mem_total_kb = total; g_mem_avail_kb = avail; }
}

static void read_loadavg(void)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp) return;
    double l1 = 0, l5 = 0, l15 = 0;
    int n = fscanf(fp, "%lf %lf %lf", &l1, &l5, &l15);
    fclose(fp);
    if (n >= 3) { g_load1 = l1; g_load5 = l5; g_load15 = l15; }
}

static void read_temperature(void)
{
    /* thermal_zone0 is CPU on Pi; may not exist on x86 hosts with no
     * ACPI thermal zones exposed, in which case we leave g_temp_c at 0. */
    FILE *fp = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!fp) return;
    long t = 0;
    int n = fscanf(fp, "%ld", &t);
    fclose(fp);
    if (n == 1) g_temp_c = (double)t / 1000.0;
}

static void autodetect_iface(void)
{
    /* First up, non-loopback iface from /proc/net/route header row */
    FILE *fp = fopen("/proc/net/route", "r");
    if (!fp) return;
    char line[256];
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }
    while (fgets(line, sizeof(line), fp)) {
        char iface[MAX_IFACE];
        if (sscanf(line, "%31s", iface) == 1 && strcmp(iface, "lo") != 0) {
            snprintf(g_iface, sizeof(g_iface), "%s", iface);
            break;
        }
    }
    fclose(fp);
}

static void read_network(void)
{
    if (!g_iface[0]) return;
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) return;
    char line[512];
    /* skip two header lines */
    if (!fgets(line, sizeof(line), fp) || !fgets(line, sizeof(line), fp)) {
        fclose(fp); return;
    }
    uint64_t rx_bytes = 0, tx_bytes = 0;
    int found = 0;
    size_t iflen = strlen(g_iface);
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ') p++;
        if (strncmp(p, g_iface, iflen) == 0 && p[iflen] == ':') {
            p += iflen + 1;
            unsigned long long rxb, rxp, rxe, rxd, rxf, rxc, rxcomp, rxmc;
            unsigned long long txb;
            if (sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &rxb, &rxp, &rxe, &rxd, &rxf, &rxc, &rxcomp, &rxmc,
                       &txb) >= 9) {
                rx_bytes = rxb;
                tx_bytes = txb;
                found = 1;
            }
            break;
        }
    }
    fclose(fp);
    if (!found) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (g_have_prev_net) {
        double dt = (ts.tv_sec - g_prev_net_ts.tv_sec) +
                    (ts.tv_nsec - g_prev_net_ts.tv_nsec) / 1e9;
        if (dt > 0) {
            uint64_t drx = rx_bytes - g_prev_net_rx;
            uint64_t dtx = tx_bytes - g_prev_net_tx;
            g_net_rx_bps = (uint64_t)((double)drx * 8.0 / dt);
            g_net_tx_bps = (uint64_t)((double)dtx * 8.0 / dt);
        }
    }
    g_prev_net_rx = rx_bytes;
    g_prev_net_tx = tx_bytes;
    g_prev_net_ts = ts;
    g_have_prev_net = 1;
}

/* ───────────────────────── RRD open/close ───────────────────────── */

static int rrd_open(const char *path)
{
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "cannot open %s: %s", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }

    int fresh = (st.st_size < (off_t)sizeof(sys_rrd_t));
    if (fresh && ftruncate(fd, sizeof(sys_rrd_t)) != 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "ftruncate %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    void *mm = mmap(NULL, sizeof(sys_rrd_t),
                    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mm == MAP_FAILED) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "mmap %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }

    g_rrd    = mm;
    g_rrd_fd = fd;

    /* Initialize or refuse stale magic/version — wipe and start over
     * rather than try to migrate an old layout. Sysstats history is
     * diagnostic; losing it on a version bump is acceptable. */
    if (g_rrd->magic != SYS_RRD_MAGIC || g_rrd->version != SYS_RRD_VERSION) {
        memset(g_rrd, 0, sizeof(*g_rrd));
        g_rrd->magic      = SYS_RRD_MAGIC;
        g_rrd->version    = SYS_RRD_VERSION;
        g_rrd->start_time = (int64_t)time(NULL);
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "initialized sysstats RRD: %s", path);
    } else {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "opened sysstats RRD: %s (%llu samples)", path,
                    (unsigned long long)g_rrd->total_samples);
    }
    return 0;
}

static void rrd_close(void)
{
    if (g_rrd && g_rrd != MAP_FAILED) {
        msync(g_rrd, sizeof(*g_rrd), MS_SYNC);
        munmap(g_rrd, sizeof(*g_rrd));
    }
    if (g_rrd_fd >= 0) close(g_rrd_fd);
    g_rrd    = NULL;
    g_rrd_fd = -1;
}

/* ───────────────────────── Accumulators ──────────────────────────── */

static void acc_reset(sys_acc_t *a) { memset(a, 0, sizeof(*a)); }

static void acc_feed_current(sys_acc_t *a)
{
    double mem_pct = 0;
    if (g_mem_total_kb > 0)
        mem_pct = 100.0 *
                  (double)(g_mem_total_kb - g_mem_avail_kb) /
                  (double)g_mem_total_kb;
    a->sum_cpu  += g_cpu_pct;
    a->sum_mem  += mem_pct;
    a->sum_temp += g_temp_c;
    a->sum_l1   += g_load1;
    a->sum_l5   += g_load5;
    a->sum_l15  += g_load15;
    a->sum_rx   += (double)g_net_rx_bps;
    a->sum_tx   += (double)g_net_tx_bps;
    a->count++;
}

static void acc_to_slot(const sys_acc_t *a, sys_slot_t *s, time_t t)
{
    if (a->count == 0) { s->t = 0; return; }
    double n = (double)a->count;
    s->t            = (int64_t)t;
    s->cpu_pct      = (float)(a->sum_cpu  / n);
    s->mem_used_pct = (float)(a->sum_mem  / n);
    s->temp_c       = (float)(a->sum_temp / n);
    s->load1        = (float)(a->sum_l1   / n);
    s->load5        = (float)(a->sum_l5   / n);
    s->load15       = (float)(a->sum_l15  / n);
    s->net_rx_bps   = (float)(a->sum_rx   / n);
    s->net_tx_bps   = (float)(a->sum_tx   / n);
}

/* On every sample, compare wall clock to the previous sample. If we've
 * crossed minute/hour/day boundaries, commit the corresponding
 * accumulator into its RRD slot and reset. Slot index is derived
 * directly from epoch — e.g. (minute_epoch/60) % 60 — so a daemon
 * outage just leaves older ring entries with mismatched `t` which the
 * publisher filters out on read. */
static void handle_rollovers(time_t now)
{
    if (!g_rrd) return;
    if (g_last_rollover_t == 0) { g_last_rollover_t = now; return; }

    time_t last      = g_last_rollover_t;
    time_t last_min  = last - (last % 60);
    time_t cur_min   = now  - (now  % 60);
    time_t last_hour = last - (last % 3600);
    time_t cur_hour  = now  - (now  % 3600);
    time_t last_day  = last - (last % 86400);
    time_t cur_day   = now  - (now  % 86400);

    if (cur_min > last_min) {
        int idx = (int)((last_min / 60) % SYS_RRD_MINS);
        acc_to_slot(&g_min_acc, &g_rrd->minutes[idx], last_min);
        acc_reset(&g_min_acc);
    }
    if (cur_hour > last_hour) {
        int idx = (int)((last_hour / 3600) % SYS_RRD_HOURS);
        acc_to_slot(&g_hour_acc, &g_rrd->hours[idx], last_hour);
        acc_reset(&g_hour_acc);
    }
    if (cur_day > last_day) {
        int idx = (int)((last_day / 86400) % SYS_RRD_DAYS);
        acc_to_slot(&g_day_acc, &g_rrd->days[idx], last_day);
        acc_reset(&g_day_acc);
    }

    g_last_rollover_t = now;
}

/* ───────────────────── History publish (SSE) ─────────────────────── */

/* Append one slot as JSON to dst; "null" if the slot's timestamp is
 * missing or older than the ring's horizon (i.e. stale from a previous
 * wrap-around). Caller already wrote any separator. */
static int append_slot_json(char *dst, size_t cap, const sys_slot_t *s,
                             time_t horizon_oldest)
{
    if (s->t <= 0 || s->t < (int64_t)horizon_oldest)
        return snprintf(dst, cap, "null");
    return snprintf(dst, cap,
        "{\"t\":%lld,\"cpu_pct\":%.2f,\"mem_used_pct\":%.2f,"
        "\"temp_c\":%.2f,\"load1\":%.3f,\"load5\":%.3f,\"load15\":%.3f,"
        "\"net_rx_bps\":%.0f,\"net_tx_bps\":%.0f}",
        (long long)s->t,
        s->cpu_pct, s->mem_used_pct, s->temp_c,
        s->load1, s->load5, s->load15,
        s->net_rx_bps, s->net_tx_bps);
}

static void publish_sys_history(void)
{
    if (!g_core || !g_core->sse_publish || !g_rrd) return;

    time_t now      = time(NULL);
    time_t cur_min  = now - (now % 60);
    time_t cur_hour = now - (now % 3600);
    time_t cur_day  = now - (now % 86400);

    /* 114 slots * ~220 B json each + separators + scaffolding ≈ 30 KB.
     * Oversize to 48 KB so we never truncate. Freed right after send. */
    size_t cap = 48 * 1024;
    char  *buf = malloc(cap);
    if (!buf) return;
    size_t n = 0;

    /* Small inline helper: append a literal to buf, bail out on overflow. */
    #define APPEND_LIT(lit) do { \
        size_t _len = sizeof(lit) - 1; \
        if (n + _len >= cap) { free(buf); return; } \
        memcpy(buf + n, lit, _len); n += _len; \
    } while (0)

    APPEND_LIT("{\"minutely\":[");
    {
        time_t horizon = cur_min - (time_t)(SYS_RRD_MINS - 1) * 60;
        /* Emit oldest→newest across the 60-slot ring. */
        for (int i = 0; i < SYS_RRD_MINS; i++) {
            int idx = (int)(((cur_min / 60) + 1 + i) % SYS_RRD_MINS);
            if (i) APPEND_LIT(",");
            int w = append_slot_json(buf + n, cap - n,
                                      &g_rrd->minutes[idx], horizon);
            if (w < 0 || (size_t)w >= cap - n) { free(buf); return; }
            n += (size_t)w;
        }
    }
    APPEND_LIT("],\"hourly\":[");
    {
        time_t horizon = cur_hour - (time_t)(SYS_RRD_HOURS - 1) * 3600;
        for (int i = 0; i < SYS_RRD_HOURS; i++) {
            int idx = (int)(((cur_hour / 3600) + 1 + i) % SYS_RRD_HOURS);
            if (i) APPEND_LIT(",");
            int w = append_slot_json(buf + n, cap - n,
                                      &g_rrd->hours[idx], horizon);
            if (w < 0 || (size_t)w >= cap - n) { free(buf); return; }
            n += (size_t)w;
        }
    }
    APPEND_LIT("],\"daily\":[");
    {
        time_t horizon = cur_day - (time_t)(SYS_RRD_DAYS - 1) * 86400;
        for (int i = 0; i < SYS_RRD_DAYS; i++) {
            int idx = (int)(((cur_day / 86400) + 1 + i) % SYS_RRD_DAYS);
            if (i) APPEND_LIT(",");
            int w = append_slot_json(buf + n, cap - n,
                                      &g_rrd->days[idx], horizon);
            if (w < 0 || (size_t)w >= cap - n) { free(buf); return; }
            n += (size_t)w;
        }
    }
    APPEND_LIT("]}");
    if (n < cap) buf[n] = '\0';

    #undef APPEND_LIT

    g_core->sse_publish("sys_history_updated", buf, /*admin_only=*/1);
    free(buf);
}

static void publish_sys_history_cb(void *ud) { (void)ud; publish_sys_history(); }

/* Populate a kerchunk_resp_t with the current sample. Used by both the
 * `sys` CLI handler and the periodic SSE publish after each sample. */
static void render_sys_resp(kerchunk_resp_t *r)
{
    resp_bool(r, "enabled", g_enabled);
    resp_int(r, "interval_ms", g_interval_ms);
    resp_str(r, "iface", g_iface);
    resp_bool(r, "have_sample", g_have_sample);
    resp_float(r, "cpu_pct", g_cpu_pct);
    resp_int(r, "mem_total_kb", (int)g_mem_total_kb);
    resp_int(r, "mem_avail_kb", (int)g_mem_avail_kb);
    if (g_mem_total_kb > 0) {
        double used_pct = 100.0 *
            (double)(g_mem_total_kb - g_mem_avail_kb) / (double)g_mem_total_kb;
        resp_float(r, "mem_used_pct", used_pct);
    }
    resp_float(r, "temp_c", g_temp_c);
    resp_float(r, "load1", g_load1);
    resp_float(r, "load5", g_load5);
    resp_float(r, "load15", g_load15);
    resp_int64(r, "net_rx_bps", (int64_t)g_net_rx_bps);
    resp_int64(r, "net_tx_bps", (int64_t)g_net_tx_bps);
    resp_int64(r, "sample_time", (int64_t)g_last_sample_time);
}

static void sample_cb(void *ud)
{
    (void)ud;
    read_cpu();
    read_meminfo();
    read_loadavg();
    read_temperature();
    read_network();
    g_last_sample_time = time(NULL);
    g_have_sample = 1;

    /* Feed the three independent horizon accumulators. Each 5s sample
     * bumps all of them so minute / hour / day averages are each a
     * true mean of raw samples, not a mean-of-means. */
    acc_feed_current(&g_min_acc);
    acc_feed_current(&g_hour_acc);
    acc_feed_current(&g_day_acc);
    handle_rollovers(g_last_sample_time);
    if (g_rrd) g_rrd->total_samples++;

    /* Broadcast the fresh sample to admin SSE clients. Payload is the
     * same JSON the CLI would emit, so consumers see a consistent shape
     * whether they polled or subscribed. */
    if (g_core->sse_publish) {
        kerchunk_resp_t r;
        resp_init(&r);
        render_sys_resp(&r);
        resp_finish(&r);
        g_core->sse_publish("sys_updated", r.json, /*admin_only=*/1);
    }
}

/* CLI */
static int cli_sys(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "help") == 0) goto usage;
    render_sys_resp(r);
    return 0;

usage:
    resp_text_raw(r, "Host system metrics\n\n"
        "  sys                  Show latest sample (JSON)\n\n"
        "    Fields:\n"
        "      cpu_pct          Total CPU utilization (%)\n"
        "      mem_total_kb     Total RAM in KB\n"
        "      mem_avail_kb     Available RAM in KB\n"
        "      mem_used_pct     Memory utilization (%)\n"
        "      temp_c           CPU temperature (deg C)\n"
        "      load1/5/15       Load average over 1, 5, 15 minutes\n"
        "      net_rx_bps       Network RX bits/sec on iface\n"
        "      net_tx_bps       Network TX bits/sec on iface\n"
        "      iface            Network interface being monitored\n\n"
        "Config: [sysstats] enabled=on|off, interval=5s, iface=eth0\n");
    resp_str(r, "error", "usage: sys [help]");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { .name = "sys", .usage = "sys", .description = "Host system metrics",
      .handler = cli_sys, .category = "System" },
};

/* Lifecycle */

static int sysstats_load(kerchunk_core_t *core)
{
    g_core = core;
    return 0;
}

static int sysstats_configure(const kerchunk_config_t *cfg)
{
    const char *v = kerchunk_config_get(cfg, "sysstats", "enabled");
    g_enabled = (!v || strcmp(v, "off") != 0);  /* default on */

    g_interval_ms = kerchunk_config_get_duration_ms(cfg, "sysstats",
                                                     "interval", 5000);
    if (g_interval_ms < 1000) g_interval_ms = 1000;

    v = kerchunk_config_get(cfg, "sysstats", "iface");
    if (v && *v)
        snprintf(g_iface, sizeof(g_iface), "%s", v);
    else if (!g_iface[0])
        autodetect_iface();

    v = kerchunk_config_get(cfg, "sysstats", "rrd_file");
    if (v && *v) snprintf(g_rrd_path, sizeof(g_rrd_path), "%s", v);

    /* First-sample CPU/net deltas need two consecutive reads — reset the
     * has-previous flags so a config reload doesn't produce a negative or
     * multi-hour spike from stale deltas. */
    g_have_prev_cpu = 0;
    g_have_prev_net = 0;
    /* Reset in-flight accumulators too; the partial bucket from the
     * previous configure isn't meaningful after a restart. */
    acc_reset(&g_min_acc);
    acc_reset(&g_hour_acc);
    acc_reset(&g_day_acc);
    g_last_rollover_t = 0;

    /* Open the persistent history RRD. Diagnostic data — if we can't
     * open it, sys_updated keeps working without history. */
    if (g_rrd) rrd_close();
    rrd_open(g_rrd_path);

    if (g_sched_id >= 0) {
        g_core->schedule_cancel(g_sched_id);
        g_sched_id = -1;
    }
    if (g_history_sched >= 0) {
        g_core->schedule_cancel(g_history_sched);
        g_history_sched = -1;
    }

    if (g_enabled) {
        g_sched_id = g_core->schedule_aligned(g_interval_ms, 0, 1,
                                              sample_cb, NULL);
        /* Prime a first read so the delta counters have a baseline; the
         * next scheduled tick produces the first real CPU%/net rate. */
        sample_cb(NULL);

        /* Wall-clock-aligned 60s history publish — matches the RRD
         * minute rotation so each push includes the slot that just
         * completed. */
        g_history_sched = g_core->schedule_aligned(60000, 0, 1,
                                                    publish_sys_history_cb, NULL);
        /* Seed snapshot cache so reconnecting pages get the current
         * (mostly empty on first boot) history immediately. */
        publish_sys_history();
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "enabled=%d interval=%dms iface=%s rrd=%s",
                g_enabled, g_interval_ms,
                g_iface[0] ? g_iface : "(none)",
                g_rrd ? g_rrd_path : "(unavailable)");
    return 0;
}

static void sysstats_unload(void)
{
    if (g_sched_id >= 0) {
        g_core->schedule_cancel(g_sched_id);
        g_sched_id = -1;
    }
    if (g_history_sched >= 0) {
        g_core->schedule_cancel(g_history_sched);
        g_history_sched = -1;
    }
    rrd_close();
}

static kerchunk_module_def_t mod_sysstats = {
    .name             = "mod_sysstats",
    .version          = "1.0.0",
    .description      = "Host system metrics (CPU, memory, temperature, network)",
    .load             = sysstats_load,
    .configure        = sysstats_configure,
    .unload           = sysstats_unload,
    .cli_commands     = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_sysstats);
