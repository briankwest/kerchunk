/*
 * main.c — kerchunkd daemon entry point
 *
 * Two-thread architecture:
 *   Audio thread (20ms) — capture, DSP decoders, queue drain, playback
 *   Main thread  (20ms) — timers, socket, COR, config reload
 *
 * Both threads fire events on the shared (mutex-protected) event bus.
 */

#include "kerchunk.h"
#include "kerchunk_log.h"
#include "kerchunk_console.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <getopt.h>
#include <pthread.h>

#define LOG_MOD "main"

/* Audio thread cadence */
#define AUDIO_TICK_US 20000  /* 20ms — matches PortAudio callback cadence and frame size */

extern kerchunk_core_t *kerchunk_core_get(void);
extern void kerchunk_core_set_config(kerchunk_config_t *cfg);
extern void kerchunk_core_set_cor(int active);
extern int  kerchunk_core_get_ptt(void);
extern void kerchunk_core_dispatch_taps(const kerchevt_t *evt);
extern void kerchunk_core_dispatch_playback_taps(const kerchevt_t *evt);
extern void kerchunk_timer_init(void);
extern void kerchunk_timer_shutdown(void);
extern void kerchunk_timer_tick(void);

extern int  kerchunk_socket_init(const char *path);
extern void kerchunk_socket_shutdown(void);
extern void kerchunk_socket_poll(void);

#include "plcode.h"

volatile sig_atomic_t g_running = 1;  /* non-static: console thread sets to 0 */
static volatile sig_atomic_t g_reload  = 0;
static time_t g_start_time = 0;  /* Daemon start — never resets */

static int g_sample_rate   = 48000;
static int g_frame_samples = 960;
static void handle_signal(int sig)
{
    if (sig == SIGTERM || sig == SIGINT)
        g_running = 0;
    else if (sig == SIGHUP)
        g_reload = 1;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [-c config] [-f] [-d] [-h]\n", prog);
    fprintf(stderr, "  -c config   Config file (default: kerchunk.conf)\n");
    fprintf(stderr, "  -f          Run in foreground\n");
    fprintf(stderr, "  -d          List audio devices and exit\n");
    fprintf(stderr, "  -h          Show this help\n");
}

/* ════════════════════════════════════════════════════════════════════
 *  CLI command handlers (called from main thread via socket)
 * ════════════════════════════════════════════════════════════════════ */

static const char *get_tx_state(void);  /* Forward declaration — defined after audio globals */

/* Expose RX state from mod_repeater for the unified status endpoint.
 * mod_repeater's g_state is static, but the module's CLI handler returns
 * the state string. We call it via dispatch. */
static const char *get_rx_state(void)
{
    kerchunk_resp_t tmp;
    resp_init(&tmp);
    const char *argv[] = { "repeater" };
    kerchunk_module_dispatch_cli("repeater", 1, argv, &tmp);
    resp_finish(&tmp);

    /* Parse state from the JSON: {"state":"IDLE",...} */
    static char rx_state[16];
    const char *p = strstr(tmp.json, "\"state\":\"");
    if (p) {
        p += 9;
        int i = 0;
        while (*p && *p != '"' && i < 15) rx_state[i++] = *p++;
        rx_state[i] = '\0';
        return rx_state;
    }
    return "IDLE";
}

static int cmd_status(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    int ptt = kerchunk_core_get_ptt();
    int cor = kerchunk_core_get()->is_receiving();

    const kerchunk_config_t *cfg = kerchunk_core_get_config();

    resp_str(r, "version", KERCHUNK_VERSION_STRING);
    resp_str(r, "rx_state", get_rx_state());
    resp_str(r, "tx_state", get_tx_state());
    resp_bool(r, "ptt", ptt);
    resp_bool(r, "cor", cor);
    resp_int(r, "queue", kerchunk_queue_depth());
    resp_int(r, "modules", kerchunk_module_count());
    resp_int(r, "users", kerchunk_user_count());
    resp_bool(r, "emergency", kerchunk_core_get_emergency());
    if (g_start_time > 0) {
        uint64_t uptime_ms = (uint64_t)difftime(time(NULL), g_start_time) * 1000;
        resp_int64(r, "uptime_ms", (int64_t)uptime_ms);
    }

    /* Site info from config */
    const char *v;
    v = kerchunk_config_get(cfg, "general", "callsign");
    if (v) resp_str(r, "callsign", v);
    v = kerchunk_config_get(cfg, "general", "frequency");
    if (v) resp_str(r, "frequency", v);
    v = kerchunk_config_get(cfg, "general", "offset");
    if (v) resp_str(r, "offset", v);
    v = kerchunk_config_get(cfg, "general", "address");
    if (v) resp_str(r, "address", v);
    v = kerchunk_config_get(cfg, "general", "latitude");
    if (v) resp_str(r, "latitude", v);
    v = kerchunk_config_get(cfg, "general", "longitude");
    if (v) resp_str(r, "longitude", v);
    v = kerchunk_config_get(cfg, "general", "elevation");
    if (v) resp_str(r, "elevation", v);
    /* google_maps_api_key intentionally excluded from public status —
     * admin pages fetch it via /admin/api/status which appends it. */
    return 0;
}

static int cmd_ptt(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc < 2) { resp_str(r, "error", "Usage: ptt on|off"); return -1; }
    kerchunk_core_t *core = kerchunk_core_get();
    if (strcmp(argv[1], "on") == 0) {
        core->request_ptt("cli");
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "ptt_on");
    } else {
        core->release_ptt("cli");
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "ptt_off");
    }
    return 0;
}

static int cmd_queue(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc < 2 || (argc >= 2 && strcmp(argv[1], "help") == 0)) {
        resp_text_raw(r, "Audio queue management\n\n"
            "  queue list                 Show queue depth\n"
            "  queue inject <path.wav>    Queue a WAV/PCM file for playback\n"
            "  queue flush                Clear all queued items\n");
        resp_str(r, "error", "usage: queue list|inject <path>|flush");
        return -1;
    }
    if (strcmp(argv[1], "list") == 0) {
        int depth = kerchunk_queue_depth();
        resp_int(r, "depth", depth);
    } else if (strcmp(argv[1], "inject") == 0 && argc >= 3) {
        const char *path = argv[2];
        if (strstr(path, "..") != NULL) {
            resp_str(r, "error", "path traversal not allowed");
            return -1;
        }
        size_t plen = strlen(path);
        if (plen < 4 ||
            (strcmp(path + plen - 4, ".wav") != 0 &&
             strcmp(path + plen - 4, ".pcm") != 0)) {
            resp_str(r, "error", "only .wav and .pcm files allowed");
            return -1;
        }
        int id = kerchunk_queue_add_file(path, 0);
        if (id >= 0) {
            resp_bool(r, "ok", 1);
            resp_int(r, "id", id);
            resp_str(r, "path", path);
            resp_int(r, "priority", 0);
        } else {
            resp_str(r, "error", "Failed to queue file");
        }
    } else if (strcmp(argv[1], "flush") == 0) {
        int flushed = kerchunk_queue_flush();
        resp_bool(r, "ok", 1);
        resp_int(r, "flushed", flushed);
    } else {
        resp_str(r, "error", "Usage: queue list|inject <path>|flush");
    }
    return 0;
}

static int cmd_module(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc < 2 || (argc >= 2 && strcmp(argv[1], "help") == 0)) {
        resp_text_raw(r, "Module management\n\n"
            "  module list                List loaded modules\n"
            "  module load <name>         Load a module (e.g. mod_parrot)\n"
            "  module unload <name>       Unload a module\n"
            "  module reload <name>       Unload and reload a module\n");
        resp_str(r, "error", "usage: module list|load|unload|reload <name>");
        return -1;
    }
    if (strcmp(argv[1], "list") == 0) {
        int n = kerchunk_module_count();
        /* JSON: array of module objects */
        if (!r->jfirst) resp_json_raw(r, ",");
        resp_json_raw(r, "\"modules\":[");
        for (int i = 0; i < n; i++) {
            const kerchunk_module_def_t *m = kerchunk_module_get(i);
            if (m) {
                if (i > 0) resp_json_raw(r, ",");
                char frag[256];
                snprintf(frag, sizeof(frag),
                         "{\"name\":\"%s\",\"version\":\"%s\",\"description\":\"%s\"}",
                         m->name,
                         m->version ? m->version : "",
                         m->description ? m->description : "");
                resp_json_raw(r, frag);
            }
        }
        resp_json_raw(r, "]");
        r->jfirst = 0;
        /* Text */
        for (int i = 0; i < n; i++) {
            const kerchunk_module_def_t *m = kerchunk_module_get(i);
            if (m) {
                char line[128];
                snprintf(line, sizeof(line), "%-20s %-8s %s\n",
                         m->name, m->version ? m->version : "?",
                         m->description ? m->description : "");
                resp_text_raw(r, line);
            }
        }
        if (n == 0) resp_text_raw(r, "(no modules loaded)\n");
    } else if (strcmp(argv[1], "load") == 0 && argc >= 3) {
        if (kerchunk_module_load(argv[2], kerchunk_core_get()) == 0) {
            g_reload = 1;  /* trigger configure() via main loop */
            resp_bool(r, "ok", 1);
            resp_str(r, "module", argv[2]);
        } else {
            resp_str(r, "error", "Failed to load module");
        }
    } else if (strcmp(argv[1], "unload") == 0 && argc >= 3) {
        if (kerchunk_module_unload(argv[2]) == 0) {
            resp_bool(r, "ok", 1);
            resp_str(r, "module", argv[2]);
        } else {
            resp_str(r, "error", "Failed to unload module");
        }
    } else if (strcmp(argv[1], "reload") == 0 && argc >= 3) {
        if (kerchunk_module_reload(argv[2], kerchunk_core_get()) == 0) {
            g_reload = 1;  /* trigger configure() via main loop */
            resp_bool(r, "ok", 1);
            resp_str(r, "module", argv[2]);
        } else {
            resp_str(r, "error", "Failed to reload module");
        }
    } else {
        resp_str(r, "error", "Usage: module list|load|unload|reload <name>");
    }
    return 0;
}

static int cmd_event(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    static const char *names[] = {
        "AUDIO_FRAME", "CTCSS_DETECT", "DCS_DETECT", "DTMF_DIGIT", "DTMF_END",
        "COR_ASSERT", "COR_DROP", "PTT_ASSERT", "PTT_DROP",
        "STATE_CHANGE", "TAIL_START", "TAIL_EXPIRE", "TIMEOUT",
        "CALLER_IDENTIFIED", "CALLER_CLEARED",
        "QUEUE_DRAIN", "QUEUE_COMPLETE", "RECORDING_SAVED", "ANNOUNCEMENT",
        "CONFIG_RELOAD", "SHUTDOWN", "TICK", "HEARTBEAT",
    };
    /* JSON: array of subscription objects */
    if (!r->jfirst) resp_json_raw(r, ",");
    resp_json_raw(r, "\"subscriptions\":[");
    int jfirst = 1;
    int has_text = 0;
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        int n = kerchevt_subscriber_count((kerchevt_type_t)i);
        if (n > 0) {
            if (!jfirst) resp_json_raw(r, ",");
            char frag[128];
            snprintf(frag, sizeof(frag),
                     "{\"event\":\"%s\",\"count\":%d}", names[i], n);
            resp_json_raw(r, frag);
            jfirst = 0;

            char line[64];
            snprintf(line, sizeof(line), "%-24s %d subscriber%s\n",
                     names[i], n, n == 1 ? "" : "s");
            resp_text_raw(r, line);
            has_text = 1;
        }
    }
    resp_json_raw(r, "]");
    r->jfirst = 0;
    if (!has_text) resp_text_raw(r, "(no subscriptions)\n");
    return 0;
}

static int cmd_config(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    g_reload = 1;
    resp_bool(r, "ok", 1);
    resp_str(r, "action", "config_reload");
    return 0;
}

static int cmd_shutdown(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    g_running = 0;
    resp_bool(r, "ok", 1);
    resp_str(r, "action", "shutdown");
    return 0;
}

static int cmd_version(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    resp_str(r, "version", KERCHUNK_VERSION_STRING);
    resp_int(r, "major", KERCHUNK_VERSION_MAJOR);
    resp_int(r, "minor", KERCHUNK_VERSION_MINOR);
    resp_int(r, "patch", KERCHUNK_VERSION_PATCH);
    resp_int(r, "sample_rate", g_sample_rate);
    resp_int(r, "frame_samples", g_frame_samples);
    resp_int(r, "max_frame_samples", KERCHUNK_MAX_FRAME_SAMPLES);
#ifdef __VERSION__
    resp_str(r, "compiler", __VERSION__);
#endif
    resp_str(r, "built", __DATE__ " " __TIME__);
    return 0;
}

static int cmd_uptime(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    if (g_start_time > 0) {
        time_t now = time(NULL);
        int secs = (int)difftime(now, g_start_time);
        int days = secs / 86400; secs %= 86400;
        int hours = secs / 3600; secs %= 3600;
        int mins = secs / 60; secs %= 60;
        char buf[64];
        snprintf(buf, sizeof(buf), "%dd %dh %dm %ds", days, hours, mins, secs);
        resp_str(r, "uptime", buf);
        resp_int64(r, "uptime_s", (int64_t)difftime(now, g_start_time));
    }
    resp_int(r, "queue_depth", kerchunk_queue_depth());
    resp_int(r, "modules", kerchunk_module_count());
    resp_int(r, "users", kerchunk_user_count());
    resp_bool(r, "ptt", kerchunk_core_get_ptt());
    resp_bool(r, "cor", kerchunk_core_get()->is_receiving());
    resp_bool(r, "emergency", kerchunk_core_get_emergency());
    return 0;
}

static int cmd_audio(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    resp_int(r, "sample_rate", g_sample_rate);
    resp_int(r, "frame_samples", g_frame_samples);
    resp_int(r, "hw_rate", kerchunk_audio_get_rate());
    resp_bool(r, "resampling", g_sample_rate != kerchunk_audio_get_rate());

    const kerchunk_config_t *cfg = kerchunk_core_get_config();
    const char *v;
    v = kerchunk_config_get(cfg, "audio", "capture_device");
    if (v) resp_str(r, "capture_device", v);
    v = kerchunk_config_get(cfg, "audio", "playback_device");
    if (v) resp_str(r, "playback_device", v);

    size_t writable = kerchunk_audio_playback_writable();
    resp_int(r, "playback_writable", (int)writable);
    return 0;
}

static int cmd_hid(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    resp_bool(r, "available", kerchunk_hid_available());
    resp_bool(r, "ptt", kerchunk_core_get_ptt());
    int cor = kerchunk_hid_read_cor();
    resp_int(r, "cor_raw", cor);
    resp_bool(r, "cor", kerchunk_core_get()->is_receiving());

    const kerchunk_config_t *cfg = kerchunk_core_get_config();
    const char *v;
    v = kerchunk_config_get(cfg, "hid", "device");
    if (v) resp_str(r, "device", v);
    v = kerchunk_config_get(cfg, "hid", "ptt_bit");
    if (v) resp_int(r, "ptt_bit", atoi(v));
    v = kerchunk_config_get(cfg, "hid", "cor_bit");
    if (v) resp_int(r, "cor_bit", atoi(v));
    v = kerchunk_config_get(cfg, "hid", "cor_polarity");
    if (v) resp_str(r, "cor_polarity", v);
    return 0;
}

static int cmd_user(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc < 2 || (argc >= 2 && strcmp(argv[1], "help") == 0)) {
        resp_text_raw(r, "User database\n\n"
            "  user list                  List all users (id, name, ANI, access)\n"
            "  user lookup <id|ani>       Look up a user by ID or ANI code\n");
        resp_str(r, "error", "usage: user list|lookup <id|ani>");
        return -1;
    }
    if (strcmp(argv[1], "list") == 0) {
        int n = kerchunk_user_count();
        resp_int(r, "count", n);
        if (!r->jfirst) resp_json_raw(r, ",");
        resp_json_raw(r, "\"users\":[");
        for (int i = 0; i < n; i++) {
            const kerchunk_user_t *u = kerchunk_user_get(i);
            if (!u) continue;
            if (i > 0) resp_json_raw(r, ",");
            char frag[256];
            snprintf(frag, sizeof(frag),
                     "{\"id\":%d,\"name\":\"%s\",\"ani\":\"%s\",\"access\":%d}",
                     u->id, u->name, u->ani, u->access);
            resp_json_raw(r, frag);

            char line[128];
            snprintf(line, sizeof(line), "  [%d] %-16s ANI=%-6s access=%d\n",
                     u->id, u->name, u->ani, u->access);
            resp_text_raw(r, line);
        }
        resp_json_raw(r, "]");
        r->jfirst = 0;
    } else if (strcmp(argv[1], "lookup") == 0 && argc >= 3) {
        const kerchunk_user_t *u = NULL;
        int id = atoi(argv[2]);
        if (id > 0)
            u = kerchunk_core_get()->user_lookup_by_id(id);
        if (!u)
            u = kerchunk_core_get()->user_lookup_by_ani(argv[2]);
        if (u) {
            resp_int(r, "id", u->id);
            resp_str(r, "name", u->name);
            resp_str(r, "ani", u->ani);
            resp_int(r, "access", u->access);
        } else {
            resp_str(r, "error", "User not found");
            return -1;
        }
    } else {
        resp_str(r, "error", "Usage: user list|lookup <id|ani>");
        return -1;
    }
    return 0;
}

extern int kerchunk_log_get_level(void);

static int cmd_log(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc < 2 || (argc >= 2 && strcmp(argv[1], "help") == 0)) {
        resp_text_raw(r, "Runtime log level control\n\n"
            "  log level                  Show current log level\n"
            "  log level <level>          Set log level\n"
            "    Levels: error, warn, info, debug\n");
        resp_str(r, "error", "usage: log level [error|warn|info|debug]");
        return -1;
    }
    if (strcmp(argv[1], "level") == 0) {
        if (argc >= 3) {
            int lvl = -1;
            if (strcmp(argv[2], "error") == 0) lvl = KERCHUNK_LOG_ERROR;
            else if (strcmp(argv[2], "warn") == 0) lvl = 4;
            else if (strcmp(argv[2], "info") == 0) lvl = KERCHUNK_LOG_INFO;
            else if (strcmp(argv[2], "debug") == 0) lvl = KERCHUNK_LOG_DEBUG;

            if (lvl < 0) {
                resp_str(r, "error", "Invalid level (error|warn|info|debug)");
                return -1;
            }
            kerchunk_log_set_level(lvl);
            resp_bool(r, "ok", 1);
            resp_str(r, "level", argv[2]);
        } else {
            /* just show current level */
            int lvl = kerchunk_log_get_level();
            const char *name = "unknown";
            if (lvl <= KERCHUNK_LOG_ERROR) name = "error";
            else if (lvl <= 4) name = "warn";
            else if (lvl <= KERCHUNK_LOG_INFO) name = "info";
            else name = "debug";
            resp_str(r, "level", name);
        }
    } else {
        resp_str(r, "error", "Usage: log level [error|warn|info|debug]");
        return -1;
    }
    return 0;
}

static int cmd_diag(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    resp_bool(r, "audio", 1);  /* if we got here, daemon is running */
    resp_bool(r, "hid", kerchunk_hid_available());
    resp_int(r, "modules", kerchunk_module_count());
    resp_int(r, "queue_depth", kerchunk_queue_depth());
    resp_bool(r, "ptt", kerchunk_core_get_ptt());
    resp_bool(r, "cor", kerchunk_core_get()->is_receiving());
    resp_int(r, "sample_rate", g_sample_rate);
    resp_bool(r, "resampling", g_sample_rate != kerchunk_audio_get_rate());
    resp_int(r, "users", kerchunk_user_count());
    resp_bool(r, "emergency", kerchunk_core_get_emergency());

    if (g_start_time > 0) {
        int secs = (int)difftime(time(NULL), g_start_time);
        char buf[64];
        snprintf(buf, sizeof(buf), "%dd %dh %dm",
                 secs / 86400, (secs % 86400) / 3600, (secs % 3600) / 60);
        resp_str(r, "uptime", buf);
    }
    return 0;
}

static int cmd_play(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc < 2 || (argc >= 2 && strcmp(argv[1], "help") == 0)) {
        resp_text_raw(r, "Play a WAV file through the repeater\n\n"
            "  play <path.wav>    Queue file for playback at normal priority.\n"
            "                     WAV files at any sample rate are auto-resampled.\n");
        resp_str(r, "error", "usage: play <path.wav>");
        return -1;
    }
    const char *path = argv[1];
    if (strstr(path, "..") != NULL) {
        resp_str(r, "error", "path traversal not allowed");
        return -1;
    }
    int id = kerchunk_queue_add_file(path, KERCHUNK_PRI_NORMAL);
    if (id >= 0) {
        resp_bool(r, "ok", 1);
        resp_str(r, "path", path);
        resp_int(r, "id", id);
    } else {
        resp_str(r, "error", "Failed to queue file");
        return -1;
    }
    return 0;
}

static int cmd_tone(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc < 3 || (argc >= 2 && strcmp(argv[1], "help") == 0)) {
        resp_text_raw(r, "Play a test tone through the repeater\n\n"
            "  tone <freq_hz> <duration_ms>\n"
            "    freq_hz:      100-10000 Hz\n"
            "    duration_ms:  10-30000 ms\n\n"
            "  Example: tone 1000 500   (1kHz for 0.5 seconds)\n");
        resp_str(r, "error", "usage: tone <freq_hz> <duration_ms>");
        return -1;
    }
    int freq = atoi(argv[1]);
    int dur = atoi(argv[2]);
    if (freq < 100 || freq > 10000) {
        resp_str(r, "error", "Frequency must be 100-10000 Hz");
        return -1;
    }
    if (dur < 10 || dur > 30000) {
        resp_str(r, "error", "Duration must be 10-30000 ms");
        return -1;
    }
    int id = kerchunk_queue_add_tone(freq, dur, 16000, KERCHUNK_PRI_NORMAL);
    if (id >= 0) {
        resp_bool(r, "ok", 1);
        resp_int(r, "freq", freq);
        resp_int(r, "duration_ms", dur);
    } else {
        resp_str(r, "error", "Failed to queue tone");
        return -1;
    }
    return 0;
}

/* ── Managed threads CLI ── */

static void threads_iter_cb(int id, const char *name, int running,
                            uint64_t start_us, void *ud)
{
    kerchunk_resp_t *r = (kerchunk_resp_t *)ud;
    uint64_t now_us_v;
    { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
      now_us_v = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL; }
    int secs = (int)((now_us_v - start_us) / 1000000ULL);
    char line[128];
    snprintf(line, sizeof(line), "  %2d  %-16s %-8s %dd %dh %dm\n",
             id, name, running ? "running" : "stopped",
             secs / 86400, (secs % 86400) / 3600, (secs % 3600) / 60);
    resp_text_raw(r, line);
}

static int cmd_threads(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    int n = kerchunk_thread_count();
    resp_int(r, "count", n);
    resp_text_raw(r, "  ID  Name              State     Uptime\n");
    kerchunk_thread_iter(threads_iter_cb, r);
    if (n == 0) resp_text_raw(r, "  (no managed threads)\n");
    return 0;
}

/* ── Schedule CLI ── */

static void sched_iter_cb(int id, const char *type,
                          const struct timespec *next, int repeat, void *ud)
{
    kerchunk_resp_t *r = (kerchunk_resp_t *)ud;
    struct tm tbuf;
    gmtime_r(&next->tv_sec, &tbuf);
    char line[128];
    snprintf(line, sizeof(line),
             "  %2d  %-10s %04d-%02d-%02d %02d:%02d:%02d.%03ld  %s\n",
             id, type,
             tbuf.tm_year + 1900, tbuf.tm_mon + 1, tbuf.tm_mday,
             tbuf.tm_hour, tbuf.tm_min, tbuf.tm_sec,
             next->tv_nsec / 1000000,
             repeat ? "repeat" : "once");
    resp_text_raw(r, line);
}

static int cmd_schedule(int argc, const char **argv, kerchunk_resp_t *r)
{
    (void)argc; (void)argv;
    int n = kerchunk_sched_count();
    resp_int(r, "count", n);
    resp_text_raw(r, "  ID  Type        Next Fire (UTC)          Repeat\n");
    kerchunk_sched_iter(sched_iter_cb, r);
    if (n == 0) resp_text_raw(r, "  (no scheduled callbacks)\n");
    return 0;
}

static int cmd_sim(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc < 2) {
        resp_text_raw(r, "Usage: sim cor on|off      Simulate COR assert/drop\n"
                         "       sim dtmf <digits>   Simulate DTMF sequence\n"
                         "       sim tx <file>       Inject audio file\n");
        resp_str(r, "error", "Usage: sim cor|dtmf|tx <args>");
        return -1;
    }

    if (strcmp(argv[1], "cor") == 0) {
        if (argc < 3) {
            resp_str(r, "error", "Usage: sim cor on|off");
            return -1;
        }
        int active = (strcmp(argv[2], "on") == 0);
        kerchunk_core_set_cor(active);
        kerchevt_t evt = {
            .type = active ? KERCHEVT_COR_ASSERT : KERCHEVT_COR_DROP,
            .timestamp_us = now_us(),
            .cor = { .active = active },
        };
        kerchevt_fire(&evt);
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "cor");
        resp_bool(r, "active", active);
    } else if (strcmp(argv[1], "dtmf") == 0) {
        if (argc < 3) {
            resp_str(r, "error", "Usage: sim dtmf <digits>");
            return -1;
        }
        const char *digits = argv[2];
        for (int i = 0; digits[i]; i++) {
            char d = digits[i];
            if (!((d >= '0' && d <= '9') || (d >= 'A' && d <= 'D') ||
                  d == '*' || d == '#')) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Skipped invalid digit: %c\n", d);
                resp_text_raw(r, msg);
                continue;
            }
            kerchevt_t on_evt = {
                .type = KERCHEVT_DTMF_DIGIT,
                .timestamp_us = now_us(),
                .dtmf = { .digit = d, .duration_ms = 100 },
            };
            kerchevt_fire(&on_evt);
            kerchevt_t off_evt = {
                .type = KERCHEVT_DTMF_END,
                .timestamp_us = now_us(),
                .dtmf = { .digit = '\0', .duration_ms = 0 },
            };
            kerchevt_fire(&off_evt);
        }
        resp_bool(r, "ok", 1);
        resp_str(r, "action", "dtmf");
        resp_str(r, "digits", digits);
    } else if (strcmp(argv[1], "tx") == 0) {
        if (argc < 3) {
            resp_str(r, "error", "Usage: sim tx <wav-file>");
            return -1;
        }
        int id = kerchunk_queue_add_file(argv[2], 0);
        if (id >= 0) {
            resp_bool(r, "ok", 1);
            resp_str(r, "action", "tx");
            resp_int(r, "id", id);
        } else {
            resp_str(r, "error", "Failed to queue file");
        }
    } else {
        resp_str(r, "error", "Unknown sim command");
        return -1;
    }
    return 0;
}

/* UI field descriptors for core FORM commands */
static const kerchunk_ui_field_t tone_fields[] = {
    { "freq",     "Frequency (Hz)", "number", NULL, "1000" },
    { "duration", "Duration (ms)",  "number", NULL, "500" },
};
static const kerchunk_ui_field_t play_fields[] = {
    { "path", "File Path", "text", NULL, "/path/to/file.wav" },
};
static const kerchunk_ui_field_t log_fields[] = {
    { "level", "Level", "select", "error,warn,info,debug", NULL },
};

static const kerchunk_cli_cmd_t g_core_cmds[] = {
    { .name = "status",   .usage = "status",                          .description = "Show daemon status",
      .handler = cmd_status },
    { .name = "version",  .usage = "version",                         .description = "Show version and build info",
      .handler = cmd_version },
    { .name = "uptime",   .usage = "uptime",                          .description = "Show uptime and summary",
      .handler = cmd_uptime },
    { .name = "audio",    .usage = "audio",                           .description = "Show audio device info",
      .handler = cmd_audio },
    { .name = "hid",      .usage = "hid",                             .description = "Show HID device status",
      .handler = cmd_hid },
    { .name = "user",     .usage = "user list|lookup <id|ani>",       .description = "User database",
      .handler = cmd_user },
    { .name = "log",      .usage = "log level [error|warn|info|debug]", .description = "Get/set log level",
      .handler = cmd_log,
      .category = "Control", .ui_label = "Log Level", .ui_type = CLI_UI_FORM,
      .ui_command = "log level", .ui_fields = log_fields, .num_ui_fields = 1 },
    { .name = "diag",     .usage = "diag",                            .description = "Quick system health check",
      .handler = cmd_diag },
    { .name = "play",     .usage = "play <path.wav>",                 .description = "Play a WAV file",
      .handler = cmd_play,
      .category = "Audio", .ui_label = "Play File", .ui_type = CLI_UI_FORM,
      .ui_command = "play", .ui_fields = play_fields, .num_ui_fields = 1 },
    { .name = "tone",     .usage = "tone <freq_hz> <duration_ms>",    .description = "Play a test tone",
      .handler = cmd_tone,
      .category = "Tones", .ui_label = "Test Tone", .ui_type = CLI_UI_FORM,
      .ui_command = "tone", .ui_fields = tone_fields, .num_ui_fields = 2 },
    { .name = "ptt",      .usage = "ptt on|off",                      .description = "Manual PTT control",
      .handler = cmd_ptt,
      .category = "Control", .ui_label = "PTT", .ui_type = CLI_UI_TOGGLE,
      .ui_command = "ptt" },
    { .name = "queue",    .usage = "queue list|inject <path>|flush",   .description = "Audio queue management",
      .handler = cmd_queue },
    { .name = "module",   .usage = "module list|load|unload|reload <name>", .description = "Module management",
      .handler = cmd_module },
    { .name = "event",    .usage = "event list",                       .description = "Show event subscriptions",
      .handler = cmd_event },
    { .name = "config",   .usage = "config reload",                    .description = "Reload configuration",
      .handler = cmd_config,
      .category = "Control", .ui_label = "Reload Config", .ui_type = CLI_UI_BUTTON,
      .ui_command = "config reload" },
    { .name = "sim",      .usage = "sim cor|dtmf|tx <args>",            .description = "Simulate radio events",
      .handler = cmd_sim },
    { .name = "threads",  .usage = "threads",                           .description = "Show managed threads",
      .handler = cmd_threads },
    { .name = "schedule", .usage = "schedule",                          .description = "Show scheduled callbacks",
      .handler = cmd_schedule },
    { .name = "shutdown", .usage = "shutdown",                          .description = "Stop the daemon",
      .handler = cmd_shutdown },
};
#define NUM_CORE_CMDS (int)(sizeof(g_core_cmds) / sizeof(g_core_cmds[0]))

extern void kerchunk_socket_set_core_commands(const kerchunk_cli_cmd_t *cmds, int count);

/* ════════════════════════════════════════════════════════════════════
 *  Audio thread — runs DSP + queue drain on a tight cadence
 * ════════════════════════════════════════════════════════════════════ */

typedef struct {
    plcode_ctcss_dec_t *ctcss_dec;
    plcode_dcs_dec_t   *dcs_dec;
    plcode_dtmf_dec_t  *dtmf_dec;
} audio_thread_ctx_t;

static int g_software_relay; /* 1 = relay RX audio to TX in software */
static int g_relay_drain;    /* Samples remaining in drain timer after COR drop */
static int g_relay_drain_ms = 500; /* Configurable relay drain time */
static int g_relay_was_active; /* Track COR transition for drain trigger */
static int g_queue_ptt;      /* 1 while the queue holds a PTT ref */
static int g_queue_fired_drain; /* 1 after QUEUE_DRAIN event fired */
static uint64_t g_queue_drain_start_us; /* timestamp when drain began */
static int g_tx_delay_ms;   /* silence after PTT assert before audio */
static int g_tx_tail_ms;    /* silence after last audio before PTT release */
static int g_tx_delay_rem;  /* remaining TX delay samples */
static int g_tx_tail_rem;   /* remaining TX tail samples */
static int g_ptt_hold_ticks; /* extra ticks after ring empty before PTT drop */

/* Derive current TX state for the status API */
static const char *get_tx_state(void)
{
    int cor = kerchunk_core_get()->is_receiving();
    if (cor && g_software_relay)
        return "TX_RELAY";
    if (g_relay_drain > 0)
        return "TX_TAIL";
    if (g_queue_ptt && g_tx_delay_rem > 0)
        return "TX_QUEUE";
    if (g_queue_ptt)
        return "TX_QUEUE";
    if (kerchunk_core_get_ptt() && !cor)
        return "TX_TAIL";
    return "TX_IDLE";
}

static void *audio_thread_fn(void *arg)
{
    audio_thread_ctx_t *ctx = (audio_thread_ctx_t *)arg;

    int prev_dtmf = 0;

    KERCHUNK_LOG_I(LOG_MOD, "audio thread started (tick=%dus)", AUDIO_TICK_US);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);

    while (g_running) {
        /* Advance deadline by exactly one tick — no drift accumulation */
        deadline.tv_nsec += AUDIO_TICK_US * 1000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }

        uint64_t t0 = now_us();

        /* ── Capture one frame from ring buffer ── */
        int16_t frame[KERCHUNK_MAX_FRAME_SAMPLES];
        int nread = kerchunk_audio_capture(frame, g_frame_samples);
        if (nread <= 0) {
            memset(frame, 0, g_frame_samples * sizeof(int16_t));
            nread = g_frame_samples;
        }

        int relay_active = kerchunk_core_get()->is_receiving();

        /* ── RX descrambler: process frame in-place before DTMF decode
         * and before events/taps. DTMF tones are in the voice band and
         * get scrambled, so the decoder needs descrambled audio. ── */
        {
            void *scr_ctx;
            kerchunk_scrambler_fn scr_fn = kerchunk_core_get_rx_scrambler(&scr_ctx);
            if (scr_fn)
                scr_fn(frame, (size_t)nread, scr_ctx);
        }

        /* Fire audio frame event (descrambled audio) */
        kerchevt_t audio_evt = {
            .type = KERCHEVT_AUDIO_FRAME,
            .timestamp_us = t0,
            .audio = { .samples = frame, .n = (size_t)nread },
        };
        kerchevt_fire(&audio_evt);
        kerchunk_core_dispatch_taps(&audio_evt);

        /* ── DTMF decoder: only process when COR active or draining ──
         * Saves CPU and prevents false detections from noise/silence.
         * The relay drain window catches late digits in the squelch tail. */
        if (relay_active || g_relay_drain > 0) {
            plcode_dtmf_result_t dtmf_res;
            plcode_dtmf_dec_process(ctx->dtmf_dec, frame, (size_t)nread, &dtmf_res);

            if (dtmf_res.detected && !prev_dtmf) {
                KERCHUNK_LOG_I(LOG_MOD, "DTMF: %c", dtmf_res.digit);
                kerchevt_t evt = {
                    .type = KERCHEVT_DTMF_DIGIT,
                    .timestamp_us = t0,
                    .dtmf = { .digit = dtmf_res.digit, .duration_ms = 0 },
                };
                kerchevt_fire(&evt);
            } else if (!dtmf_res.detected && prev_dtmf) {
                kerchevt_t evt = {
                    .type = KERCHEVT_DTMF_END,
                    .timestamp_us = t0,
                    .dtmf = { .digit = '\0', .duration_ms = 0 },
                };
                kerchevt_fire(&evt);
            }
            prev_dtmf = dtmf_res.detected;
        } else if (prev_dtmf) {
            /* COR gone and drain expired — emit final DTMF_END if
             * a tone was still held when signal dropped */
            kerchevt_t evt = {
                .type = KERCHEVT_DTMF_END,
                .timestamp_us = t0,
                .dtmf = { .digit = '\0', .duration_ms = 0 },
            };
            kerchevt_fire(&evt);
            prev_dtmf = 0;
        }

        /* ── Software relay: retransmit RX audio with TX encoder ──
         *
         * When the RT-97L's internal relay is off, kerchunkd captures RX
         * audio, mixes in the TX CTCSS/DCS tone, and writes it to the
         * playback ring buffer. This gives full software control over
         * what gets retransmitted.
         *
         * When COR drops, we don't stop instantly — we drain remaining
         * captured audio (relay_drain countdown) so the last few frames
         * of speech aren't cut off mid-word.
         */
        /* Detect COR drop → start drain countdown */
        if (g_software_relay && g_relay_was_active && !relay_active) {
            g_relay_drain = (g_sample_rate * g_relay_drain_ms) / 1000;
        }
        g_relay_was_active = relay_active;

        /* Relay when COR active OR drain timer running.
         * The drain timer gives captured audio time to flush through
         * the relay path after COR drops. Configurable via relay_drain. */
        /* Don't relay while queue is transmitting — any COR during queue TX
         * is feedback from our own transmission, not a real signal.  Without
         * this guard the relay fills the playback ring with noise and the
         * queue can never release PTT (playback_pending never reaches 0). */
        int do_relay = g_software_relay && kerchunk_core_get_ptt() &&
                       !g_queue_ptt &&
                       (relay_active || g_relay_drain > 0);

        if (do_relay && nread > 0) {
            if (kerchunk_audio_playback_writable() >= (size_t)nread) {
                int16_t relay_buf[KERCHUNK_MAX_FRAME_SAMPLES];
                memcpy(relay_buf, frame, (size_t)nread * sizeof(int16_t));

                /* TX scrambler before CTCSS/DCS mix */
                {
                    void *scr_ctx;
                    kerchunk_scrambler_fn scr_fn = kerchunk_core_get_tx_scrambler(&scr_ctx);
                    if (scr_fn)
                        scr_fn(relay_buf, (size_t)nread, scr_ctx);
                }

                kerchunk_audio_playback(relay_buf, (size_t)nread);

                /* Dispatch to playback taps (TX recording) */
                kerchevt_t relay_evt = {
                    .type = KERCHEVT_AUDIO_FRAME,
                    .audio = { .samples = relay_buf, .n = (size_t)nread },
                };
                kerchunk_core_dispatch_playback_taps(&relay_evt);

                /* Count down drain timer after COR dropped.
                 * Early stop: if captured audio is below noise floor, speech
                 * has ended — skip remaining drain so courtesy tone plays
                 * immediately instead of waiting the full relay_drain period. */
                if (!relay_active && g_relay_drain > 0) {
                    g_relay_drain -= nread;
                    int64_t pwr = 0;
                    for (int k = 0; k < nread; k++)
                        pwr += (int64_t)frame[k] * frame[k];
                    if (pwr / nread < 200 * 200)  /* ~200 RMS = noise floor */
                        g_relay_drain = 0;
                }
            }
        }

        /* ── Drain queue → playback ring buffer ──
         *
         * tx_delay: silence after PTT assert, before first audio
         * tx_tail:  silence after last audio, before PTT release
         *
         * Flow: [PTT assert] → [tx_delay silence] → [audio] → [tx_tail silence] → [PTT release]
         *
         * When software_relay is on and COR is active, queue drain is
         * paused — live relay has priority over announcements. Queue
         * resumes after COR drops.
         */

        /* Pause queue drain for live relay — but NOT if the queue already
         * holds PTT.  When queue is transmitting (g_queue_ptt=1), any COR
         * is likely TX-to-RX feedback from our own transmission and must
         * not stall the drain, or PTT gets stuck forever. */
        int queue_paused = (g_software_relay && !g_queue_ptt &&
                            (kerchunk_core_get()->is_receiving() || g_relay_drain > 0));

        /* Start PTT + delay when queue has items and we're not already playing */
        if (!queue_paused && !g_queue_ptt &&
            (kerchunk_queue_depth() > 0 || kerchunk_queue_is_draining())) {
            /* Check if PTT is already held (e.g., by mod_repeater during
             * tail/hang, or by web_ptt) BEFORE we assert our own ref —
             * otherwise get_ptt() always returns true. */
            int ptt_already_held = kerchunk_core_get_ptt();
            kerchunk_core_get()->request_ptt("queue");
            g_queue_ptt = 1;
            g_queue_fired_drain = 0;
            if (ptt_already_held)
                g_tx_delay_rem = 0;
            else
                g_tx_delay_rem = (g_sample_rate * g_tx_delay_ms) / 1000;
            g_tx_tail_rem = -1;
        }

        /* TX delay: feed silence while radio keys up */
        while (!queue_paused && g_queue_ptt && g_tx_delay_rem > 0 &&
               kerchunk_audio_playback_writable() >= (size_t)g_frame_samples) {
            int16_t silence[KERCHUNK_MAX_FRAME_SAMPLES];
            memset(silence, 0, g_frame_samples * sizeof(int16_t));
            int sn = g_tx_delay_rem < g_frame_samples ?
                     g_tx_delay_rem : g_frame_samples;

            kerchunk_audio_playback(silence, (size_t)sn);

            /* Dispatch silence through playback tap so browser stream
             * stays fed — prevents AudioWorklet de-priming during delay */
            kerchevt_t delay_evt = {
                .type = KERCHEVT_AUDIO_FRAME,
                .audio = { .samples = silence, .n = (size_t)sn },
            };
            kerchunk_core_dispatch_playback_taps(&delay_evt);

            g_tx_delay_rem -= sn;
        }

        /* Drain exactly 1 frame per tick = real-time rate (50/sec).
         * Both PortAudio and WebSocket get the same frame. No burst
         * draining — keeps the browser ring stable and prevents the
         * write pointer from lapping the read pointer. */
        int frames_drained = 0;
        if (!queue_paused && g_queue_ptt && g_tx_delay_rem <= 0 &&
            !(kerchunk_core_get()->is_receiving() && !kerchunk_core_get_ptt())) {

            int16_t play_buf[KERCHUNK_MAX_FRAME_SAMPLES];
            int nplay = kerchunk_queue_drain(play_buf, g_frame_samples);
            if (nplay > 0) {
                frames_drained = 1;

                /* Fire QUEUE_DRAIN on first audio frame */
                if (!g_queue_fired_drain) {
                    g_queue_fired_drain = 1;
                    g_queue_drain_start_us = t0;
                    kerchevt_t qd = { .type = KERCHEVT_QUEUE_DRAIN,
                                    .timestamp_us = t0 };
                    kerchevt_fire(&qd);
                }

                /* TX scrambler before CTCSS/DCS mix */
                {
                    void *scr_ctx;
                    kerchunk_scrambler_fn scr_fn = kerchunk_core_get_tx_scrambler(&scr_ctx);
                    if (scr_fn)
                        scr_fn(play_buf, (size_t)nplay, scr_ctx);
                }

                kerchunk_audio_playback(play_buf, (size_t)nplay);

                /* Dispatch to WebSocket via SPSC ring */
                kerchevt_t play_evt = {
                    .type = KERCHEVT_AUDIO_FRAME,
                    .audio = { .samples = play_buf, .n = (size_t)nplay },
                };
                kerchunk_core_dispatch_playback_taps(&play_evt);
            }
        }

        /* TX tail + PTT release when queue is empty */
        if (!queue_paused && frames_drained == 0 && g_queue_ptt && g_tx_delay_rem <= 0 &&
            kerchunk_queue_depth() == 0 && !kerchunk_queue_is_draining()) {
            /* Start tail countdown on first empty tick */
            if (g_tx_tail_rem < 0) {
                g_tx_tail_rem = (g_sample_rate * g_tx_tail_ms) / 1000;

                /* Fire QUEUE_COMPLETE at tail start — gives the dashboard
                 * time to show TAIL state before PTT drops */
                if (g_queue_fired_drain) {
                    uint64_t end_us = now_us();
                    uint32_t dur_ms = (uint32_t)((end_us - g_queue_drain_start_us) / 1000);
                    kerchevt_t qc = { .type = KERCHEVT_QUEUE_COMPLETE,
                                    .timestamp_us = end_us,
                                    .queue = { .duration_ms = dur_ms } };
                    kerchevt_fire(&qc);
                    g_queue_fired_drain = 0;
                }
            }

            /* Feed tail silence */
            while (g_tx_tail_rem > 0 &&
                   kerchunk_audio_playback_writable() >= (size_t)g_frame_samples) {
                int16_t silence[KERCHUNK_MAX_FRAME_SAMPLES];
                memset(silence, 0, g_frame_samples * sizeof(int16_t));
                int sn = g_tx_tail_rem < g_frame_samples ?
                         g_tx_tail_rem : g_frame_samples;

                kerchunk_audio_playback(silence, (size_t)sn);

                /* Dispatch silence through playback tap so browser stream
                 * stays fed — prevents AudioWorklet de-priming during tail */
                kerchevt_t tail_evt = {
                    .type = KERCHEVT_AUDIO_FRAME,
                    .audio = { .samples = silence, .n = (size_t)sn },
                };
                kerchunk_core_dispatch_playback_taps(&tail_evt);

                g_tx_tail_rem -= sn;
            }

            /* Release PTT when tail is done AND playback ring is drained
             * AND hardware has had time to flush its internal buffer.
             * Hold PTT for 3 extra ticks (60ms) after the ring empties
             * so PortAudio/ALSA can finish playing the CTCSS tail. */
            if (g_tx_tail_rem <= 0 &&
                kerchunk_audio_playback_pending() == 0) {
                if (++g_ptt_hold_ticks >= 3) {
                    g_queue_ptt = 0;
                    g_tx_tail_rem = -1;
                    g_ptt_hold_ticks = 0;
                    kerchunk_core_get()->release_ptt("queue");
                }
            } else {
                g_ptt_hold_ticks = 0;
            }
        }

        /* ── Sleep to maintain cadence (absolute time — no drift) ── */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    }

    KERCHUNK_LOG_I(LOG_MOD, "audio thread exiting");
    return NULL;
}

/* ════════════════════════════════════════════════════════════════════
 *  Main
 * ════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
    const char *config_path = "kerchunk.conf";
    int foreground = 1;

    int opt;
    while ((opt = getopt(argc, argv, "c:fdh")) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'f': foreground = 1; break;
        case 'd': kerchunk_audio_list_devices(); return 0;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    /* Signals */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* Init logging */
    g_start_time = time(NULL);
    kerchunk_log_init(KERCHUNK_LOG_DEST_STDERR, KERCHUNK_LOG_DEBUG, NULL);
    KERCHUNK_LOG_I(LOG_MOD, "kerchunkd v%s starting", KERCHUNK_VERSION_STRING);

    /* Load config */
    kerchunk_config_t *cfg = kerchunk_config_load(config_path);
    if (!cfg) {
        KERCHUNK_LOG_E(LOG_MOD, "failed to load config: %s", config_path);
        return 1;
    }
    KERCHUNK_LOG_I(LOG_MOD, "config loaded: %s", config_path);

    const char *ll = kerchunk_config_get(cfg, "general", "log_level");
    if (ll) {
        if (strcmp(ll, "error") == 0) kerchunk_log_set_level(KERCHUNK_LOG_ERROR);
        else if (strcmp(ll, "warn") == 0) kerchunk_log_set_level(KERCHUNK_LOG_WARN);
        else if (strcmp(ll, "info") == 0) kerchunk_log_set_level(KERCHUNK_LOG_INFO);
        else if (strcmp(ll, "debug") == 0) kerchunk_log_set_level(KERCHUNK_LOG_DEBUG);
    }

    /* PID file — prevents running two instances.
     * Uses POSIX fcntl locking (portable, auto-releases on crash). */
    const char *pid_path = kerchunk_config_get(cfg, "general", "pid_file");
    if (!pid_path) pid_path = "/tmp/kerchunkd.pid";
    int pid_fd = open(pid_path, O_CREAT | O_RDWR, 0644);
    if (pid_fd < 0) {
        KERCHUNK_LOG_E(LOG_MOD, "cannot open pid file: %s", pid_path);
        kerchunk_config_destroy(cfg);
        return 1;
    }
    {
        struct flock fl = {
            .l_type   = F_WRLCK,
            .l_whence = SEEK_SET,
            .l_start  = 0,
            .l_len    = 0,  /* Entire file */
        };
        if (fcntl(pid_fd, F_SETLK, &fl) < 0) {
            KERCHUNK_LOG_E(LOG_MOD,
                         "another instance is running (pid file locked: %s)",
                         pid_path);
            close(pid_fd);
            kerchunk_config_destroy(cfg);
            return 1;
        }
    }
    if (ftruncate(pid_fd, 0) < 0) { /* best-effort */ }
    dprintf(pid_fd, "%d\n", getpid());
    /* Keep pid_fd open — fcntl lock released on close/exit/crash */

    /* Init subsystems */
    kerchevt_init();
    kerchunk_timer_init();
    kerchunk_sched_init();
    kerchunk_threads_init();
    kerchunk_queue_init();
    kerchunk_core_set_config(cfg);

    /* Load users from separate file if configured, else from main config */
    const char *users_file = kerchunk_config_get(cfg, "general", "users_file");
    if (users_file && users_file[0]) {
        kerchunk_config_t *ucfg = kerchunk_config_load(users_file);
        if (ucfg) {
            kerchunk_core_set_users_config(ucfg);
            kerchunk_user_init(ucfg);
            KERCHUNK_LOG_I(LOG_MOD, "users loaded from: %s", users_file);
        } else {
            KERCHUNK_LOG_W(LOG_MOD, "users_file '%s' not found, using main config", users_file);
            kerchunk_user_init(cfg);
        }
    } else {
        kerchunk_user_init(cfg);
    }

    /* Read and validate sample rate */
    g_sample_rate = kerchunk_config_get_int(cfg, "audio", "sample_rate", 48000);
    if (g_sample_rate != 8000 && g_sample_rate != 16000 &&
        g_sample_rate != 32000 && g_sample_rate != 48000) {
        KERCHUNK_LOG_W(LOG_MOD, "invalid sample_rate %d, falling back to 48000",
                       g_sample_rate);
        g_sample_rate = 48000;
    }
    g_frame_samples = (g_sample_rate * KERCHUNK_FRAME_MS) / 1000;
    kerchunk_queue_set_rate(g_sample_rate);
    kerchunk_core_set_sample_rate(g_sample_rate);
    KERCHUNK_LOG_I(LOG_MOD, "sample_rate=%d frame_samples=%d",
                   g_sample_rate, g_frame_samples);

    /* Init audio */
    kerchunk_audio_config_t audio_cfg = {
        .capture_device  = kerchunk_config_get(cfg, "audio", "capture_device"),
        .playback_device = kerchunk_config_get(cfg, "audio", "playback_device"),
        .sample_rate     = g_sample_rate,
        .hw_rate         = kerchunk_config_get_int(cfg, "audio", "hw_rate", 0),
        .preemphasis     = 0,
        .preemphasis_alpha = 0.95f,
        .speaker_volume  = kerchunk_config_get_int(cfg, "audio", "speaker_volume", -1),
        .mic_volume      = kerchunk_config_get_int(cfg, "audio", "mic_volume", -1),
        .agc             = -1,
    };
    if (!audio_cfg.capture_device)  audio_cfg.capture_device  = "default";
    if (!audio_cfg.playback_device) audio_cfg.playback_device = "default";

    const char *agc = kerchunk_config_get(cfg, "audio", "agc");
    if (agc) audio_cfg.agc = (strcmp(agc, "on") == 0) ? 1 : 0;

    const char *pe = kerchunk_config_get(cfg, "audio", "preemphasis");
    if (pe && strcmp(pe, "on") == 0) {
        audio_cfg.preemphasis = 1;
        audio_cfg.preemphasis_alpha = kerchunk_config_get_float(cfg, "audio",
                                                               "preemphasis_alpha", 0.95f);
    }
    kerchunk_audio_init(&audio_cfg);

    /* Init HID */
    kerchunk_hid_config_t hid_cfg = {
        .device       = kerchunk_config_get(cfg, "hid", "device"),
        .cor_bit      = kerchunk_config_get_int(cfg, "hid", "cor_bit", 0),
        .cor_polarity = 0,
        .ptt_bit      = kerchunk_config_get_int(cfg, "hid", "ptt_bit", 2),
    };
    const char *pol = kerchunk_config_get(cfg, "hid", "cor_polarity");
    if (pol && strcmp(pol, "active_low") == 0) hid_cfg.cor_polarity = 1;
    if (hid_cfg.device) kerchunk_hid_init(&hid_cfg);

    /* Init control socket */
    const char *sock_path = kerchunk_config_get(cfg, "general", "socket_path");
    kerchunk_socket_init(sock_path);
    kerchunk_socket_set_core_commands(g_core_cmds, NUM_CORE_CMDS);

    /* Load modules */
    const char *mod_path = kerchunk_config_get(cfg, "modules", "module_path");
#ifndef KERCHUNK_MODULE_DIR
#define KERCHUNK_MODULE_DIR "./modules"
#endif
    kerchunk_modules_init(mod_path ? mod_path : KERCHUNK_MODULE_DIR);

    const char *mod_list = kerchunk_config_get(cfg, "modules", "load");
    if (mod_list) {
        char list_copy[512];
        snprintf(list_copy, sizeof(list_copy), "%s", mod_list);
        char *saveptr = NULL;
        char *tok = strtok_r(list_copy, ",", &saveptr);
        while (tok) {
            while (*tok == ' ') tok++;
            char *end = tok + strlen(tok) - 1;
            while (end > tok && *end == ' ') *end-- = '\0';
            if (*tok)
                kerchunk_module_load(tok, kerchunk_core_get());
            tok = strtok_r(NULL, ",", &saveptr);
        }
    }

    for (int i = 0; i < kerchunk_module_count(); i++) {
        const kerchunk_module_def_t *m = kerchunk_module_get(i);
        if (m && m->configure) m->configure(cfg);
    }

    /* TX timing (with validation) */
    g_tx_delay_ms = kerchunk_config_get_duration_ms(cfg, "repeater", "tx_delay", 100);
    g_tx_tail_ms  = kerchunk_config_get_duration_ms(cfg, "repeater", "tx_tail", 200);
    if (g_tx_delay_ms < 0) g_tx_delay_ms = 0;
    if (g_tx_delay_ms > 2000) g_tx_delay_ms = 2000;
    if (g_tx_tail_ms < 0)  g_tx_tail_ms = 0;
    if (g_tx_tail_ms > 2000) g_tx_tail_ms = 2000;
    g_tx_tail_rem = -1;

    /* Software relay: if on, kerchunkd relays RX audio to TX in software.
     * The radio's internal relay must be disabled. */
    const char *sr = kerchunk_config_get(cfg, "repeater", "software_relay");
    g_software_relay = (sr && strcmp(sr, "on") == 0);

    g_relay_drain_ms = kerchunk_config_get_duration_ms(cfg, "repeater", "relay_drain", 500);
    if (g_relay_drain_ms < 0) g_relay_drain_ms = 0;
    if (g_relay_drain_ms > 5000) g_relay_drain_ms = 5000;

    KERCHUNK_LOG_I(LOG_MOD, "tx_delay=%dms tx_tail=%dms software_relay=%s "
                 "relay_drain=%dms",
                 g_tx_delay_ms, g_tx_tail_ms, g_software_relay ? "on" : "off",
                 g_relay_drain_ms);

    /* Create decoders */
    audio_thread_ctx_t audio_ctx = { NULL, NULL, NULL };
    /* DTMF decoder: tuned for hardware repeaters where the radio briefly
     * drops COS during DTMF (CTCSS interrupted by tone).  Higher
     * misses_to_end rides through the dropout without false digit-end.
     * Higher min_off_frames prevents same-digit re-detection. */
    plcode_dtmf_dec_opts_t dtmf_opts = {0};
    dtmf_opts.misses_to_end = 5;    /* 100ms of silence to end digit */
    dtmf_opts.min_off_frames = 3;   /* 60ms cooldown for same digit re-detection */
    plcode_dtmf_dec_create_ex(&audio_ctx.dtmf_dec, g_sample_rate, &dtmf_opts);

    /* ── Start audio thread ── */
    pthread_t audio_tid;
    if (pthread_create(&audio_tid, NULL, audio_thread_fn, &audio_ctx) != 0) {
        KERCHUNK_LOG_E(LOG_MOD, "failed to create audio thread");
        return 1;
    }

    /* Start embedded console if running in foreground with a terminal */
    if (foreground && isatty(STDIN_FILENO))
        kerchunk_console_init();

    KERCHUNK_LOG_I(LOG_MOD, "entering main loop (foreground=%d)", foreground);

    int prev_cor = 0;
    int cor_drop_hold = 0;   /* ticks remaining before COR drop is accepted */
    int cor_drop_hold_ms = kerchunk_config_get_duration_ms(cfg, "repeater", "cor_drop_hold", 1000);
    if (cor_drop_hold_ms < 0) cor_drop_hold_ms = 0;
    if (cor_drop_hold_ms > 5000) cor_drop_hold_ms = 5000;
    int cor_drop_hold_ticks = cor_drop_hold_ms / 20;  /* convert ms to 20ms ticks */


    /* ── Main loop: 20ms tick — timers, socket, COR, config ── */
    while (g_running) {
        uint64_t tick_start = now_us();

        /* Config reload */
        if (g_reload) {
            g_reload = 0;
            KERCHUNK_LOG_I(LOG_MOD, "reloading config...");
            kerchunk_core_lock_config();
            if (kerchunk_config_reload(cfg) == 0) {
                /* Reload users from separate file or main config */
                kerchunk_config_t *ucfg = kerchunk_core_get_users_config();
                if (ucfg && ucfg != cfg)
                    kerchunk_config_reload(ucfg);
                kerchunk_user_init(ucfg ? ucfg : cfg);

                /* Re-configure all modules with updated config */
                for (int i = 0; i < kerchunk_module_count(); i++) {
                    const kerchunk_module_def_t *m = kerchunk_module_get(i);
                    if (m && m->configure) m->configure(cfg);
                }

                /* Re-read TX timing */
                g_tx_delay_ms = kerchunk_config_get_duration_ms(cfg, "repeater", "tx_delay", 100);
                g_tx_tail_ms  = kerchunk_config_get_duration_ms(cfg, "repeater", "tx_tail", 200);
                if (g_tx_delay_ms < 0) g_tx_delay_ms = 0;
                if (g_tx_delay_ms > 2000) g_tx_delay_ms = 2000;
                if (g_tx_tail_ms < 0) g_tx_tail_ms = 0;
                if (g_tx_tail_ms > 5000) g_tx_tail_ms = 5000;

                kerchevt_t evt = { .type = KERCHEVT_CONFIG_RELOAD, .timestamp_us = tick_start };
                kerchevt_fire(&evt);
                KERCHUNK_LOG_I(LOG_MOD, "config reloaded");
            }
            kerchunk_core_unlock_config();
        }

        /* COR polling — read HID for carrier detect state changes.
         * hidraw is event-driven: returns new state on change, -1 if no change.
         *
         * COR drop is held for cor_drop_hold_ms before being accepted.
         * DTMF tones cause the RT97L to briefly drop COS (DTMF interrupts
         * CTCSS detection), which would otherwise tear down the session
         * mid-DTMF.  COR assert is processed immediately (no hold). */
        int cor = kerchunk_hid_read_cor();

        if (cor == 1 && !prev_cor) {
            KERCHUNK_LOG_I(LOG_MOD, "COR: assert (cor=%d prev=%d hold=%d)",
                          cor, prev_cor, cor_drop_hold);
            cor_drop_hold = 0;

            /* Preempt queue if it was actively playing — incoming
             * transmission takes priority over announcements.
             * Fire QUEUE_PREEMPTED so modules (CWID, etc.) can re-queue. */
            if (kerchunk_queue_is_draining() || kerchunk_queue_depth() > 0) {
                char preempt_source[QUEUE_SOURCE_MAX] = "";
                int flushed = kerchunk_queue_preempt(preempt_source,
                                                      sizeof(preempt_source));
                if (flushed > 0) {
                    KERCHUNK_LOG_I(LOG_MOD,
                        "COR preempted queue: %d items flushed (source=%s)",
                        flushed, preempt_source[0] ? preempt_source : "?");
                    kerchevt_t pe = {
                        .type = KERCHEVT_QUEUE_PREEMPTED,
                        .timestamp_us = tick_start,
                        .preempt = { .source = preempt_source,
                                     .items_flushed = flushed },
                    };
                    kerchevt_fire(&pe);
                }
            }

            kerchunk_core_set_cor(1);
            kerchevt_t cor_evt = {
                .type = KERCHEVT_COR_ASSERT,
                .timestamp_us = tick_start,
                .cor = { .active = 1 },
            };
            kerchevt_fire(&cor_evt);
            prev_cor = 1;
        } else if (cor == 0 && prev_cor) {
            KERCHUNK_LOG_I(LOG_MOD, "COR: drop seen, starting hold timer "
                          "(cor=%d prev=%d hold_ticks=%d)",
                          cor, prev_cor, cor_drop_hold_ticks);
            cor_drop_hold = cor_drop_hold_ticks;
        } else if (cor == 1 && prev_cor && cor_drop_hold > 0) {
            KERCHUNK_LOG_I(LOG_MOD, "COR: reassert during hold, canceling "
                          "(cor=%d hold_remaining=%d)",
                          cor, cor_drop_hold);
            cor_drop_hold = 0;
        }

        /* Count down and fire COR drop if hold timer expires */
        if (cor_drop_hold > 0 && --cor_drop_hold == 0) {
            KERCHUNK_LOG_I(LOG_MOD, "COR: hold timer expired, firing drop "
                          "(prev=%d)", prev_cor);
            kerchunk_core_set_cor(0);
            kerchevt_t cor_evt = {
                .type = KERCHEVT_COR_DROP,
                .timestamp_us = tick_start,
                .cor = { .active = 0 },
            };
            kerchevt_fire(&cor_evt);
            prev_cor = 0;
        }

        /* Tick event */
        kerchevt_t tick_evt = { .type = KERCHEVT_TICK, .timestamp_us = tick_start };
        kerchevt_fire(&tick_evt);

        /* Heartbeat: every 5 seconds (250 × 20ms ticks) */
        {
            static int hb_count = 0;
            if (++hb_count >= 250) {
                hb_count = 0;
                kerchevt_t hb = { .type = KERCHEVT_HEARTBEAT, .timestamp_us = tick_start };
                kerchevt_fire(&hb);
            }
        }

        /* Timers */
        kerchunk_timer_tick();
        kerchunk_sched_tick();

        /* Socket */
        kerchunk_socket_poll();

        /* Sleep */
        uint64_t elapsed = now_us() - tick_start;
        uint64_t target = KERCHUNK_FRAME_MS * 1000;
        if (elapsed < target) {
            struct timespec ts = {
                .tv_sec  = 0,
                .tv_nsec = (long)((target - elapsed) * 1000),
            };
            nanosleep(&ts, NULL);
        }
    }

    /* ── Shutdown ── */
    kerchunk_console_shutdown();

    KERCHUNK_LOG_I(LOG_MOD, "shutting down...");

    kerchevt_t shutdown_evt = { .type = KERCHEVT_SHUTDOWN, .timestamp_us = now_us() };
    kerchevt_fire(&shutdown_evt);

    /* Wait for audio thread */
    pthread_join(audio_tid, NULL);

    /* CTCSS/DCS decoders removed — not used for control logic */
    plcode_dtmf_dec_destroy(audio_ctx.dtmf_dec);

    kerchunk_threads_shutdown();
    kerchunk_sched_shutdown();
    kerchunk_modules_shutdown();
    kerchunk_socket_shutdown();
    kerchunk_hid_shutdown();
    kerchunk_audio_shutdown();
    kerchunk_queue_shutdown();
    kerchunk_timer_shutdown();
    kerchunk_user_shutdown();
    kerchevt_shutdown();
    kerchunk_config_destroy(cfg);
    kerchunk_log_shutdown();

    /* Remove PID file */
    unlink(pid_path);
    close(pid_fd);

    return 0;
}
