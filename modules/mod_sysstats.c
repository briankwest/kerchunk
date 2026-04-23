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

#define LOG_MOD "sysstats"
#define MAX_IFACE 32

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

    /* First-sample CPU/net deltas need two consecutive reads — reset the
     * has-previous flags so a config reload doesn't produce a negative or
     * multi-hour spike from stale deltas. */
    g_have_prev_cpu = 0;
    g_have_prev_net = 0;

    if (g_sched_id >= 0) {
        g_core->schedule_cancel(g_sched_id);
        g_sched_id = -1;
    }
    if (g_enabled) {
        g_sched_id = g_core->schedule_aligned(g_interval_ms, 0, 1,
                                              sample_cb, NULL);
        /* Prime a first read so the delta counters have a baseline; the
         * next scheduled tick produces the first real CPU%/net rate. */
        sample_cb(NULL);
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "enabled=%d interval=%dms iface=%s",
                g_enabled, g_interval_ms,
                g_iface[0] ? g_iface : "(none)");
    return 0;
}

static void sysstats_unload(void)
{
    if (g_sched_id >= 0) {
        g_core->schedule_cancel(g_sched_id);
        g_sched_id = -1;
    }
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
