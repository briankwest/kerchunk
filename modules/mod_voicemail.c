/*
 * mod_voicemail.c — Voicemail system
 *
 * Listens for DTMF command events to record/play/delete messages.
 * Records audio via audio tap during recording mode.
 * Stores messages as raw PCM files.
 */

#include "kerchunk.h"
#include "kerchunk_module.h"
#include "kerchunk_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#define LOG_MOD "voicemail"

/* From mod_dtmfcmd event offsets */
#define DTMF_EVT_VOICEMAIL_STATUS  (KERCHEVT_CUSTOM + 0)
#define DTMF_EVT_VOICEMAIL_RECORD  (KERCHEVT_CUSTOM + 1)
#define DTMF_EVT_VOICEMAIL_PLAY    (KERCHEVT_CUSTOM + 2)
#define DTMF_EVT_VOICEMAIL_DELETE  (KERCHEVT_CUSTOM + 3)
#define DTMF_EVT_VOICEMAIL_LIST    (KERCHEVT_CUSTOM + 4)

static kerchunk_core_t *g_core;

/* Config */
static char g_vm_dir[256] = "/var/lib/kerchunk/voicemail";
static int  g_max_messages = 20;
static int  g_max_duration_s = 60;
static int  g_enabled = 0;

/* Recording state (g_recording read by audio thread tap) */
static volatile int g_recording;
static int      g_record_user_id;
static int16_t *g_rec_buf;
static size_t   g_rec_len;
static size_t   g_rec_cap;
static int      g_rec_timer = -1;

static int g_current_caller_id;  /* Set by caller events */

/* Ensure user voicemail directory exists */
static void ensure_dir(int user_id)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%d", g_vm_dir, user_id);
    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "cannot create directory: %s", path);
}

/* Count messages for a user */
static int count_messages(int user_id)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%d", g_vm_dir, user_id);
    DIR *d = opendir(path);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strstr(ent->d_name, ".pcm"))
            count++;
    }
    closedir(d);
    return count;
}

/* Get next message number for a user */
static int next_msg_num(int user_id)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%d", g_vm_dir, user_id);
    DIR *d = opendir(path);
    if (!d) return 1;

    int max_num = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        int num;
        if (sscanf(ent->d_name, "msg_%d.pcm", &num) == 1 && num > max_num)
            max_num = num;
    }
    closedir(d);
    return max_num + 1;
}

/* Save recording */
static void save_recording(void)
{
    if (!g_rec_buf || g_rec_len == 0 || g_record_user_id <= 0)
        return;

    ensure_dir(g_record_user_id);
    int num = next_msg_num(g_record_user_id);
    char path[512];
    snprintf(path, sizeof(path), "%s/%d/msg_%04d.pcm", g_vm_dir, g_record_user_id, num);

    if (kerchunk_pcm_write(path, g_rec_buf, g_rec_len) == 0) {
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "saved message: %s (%zu samples)",
                    path, g_rec_len);
    } else {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to save: %s", path);
    }
}

/* Forward declaration */
static void rec_audio_tap(const kerchevt_t *evt, void *ud);

/* Stop recording helper (idempotent) */
static void stop_recording(void)
{
    if (!g_recording)
        return;

    g_recording = 0;
    g_core->audio_tap_unregister(rec_audio_tap);

    if (g_rec_timer >= 0) {
        g_core->timer_cancel(g_rec_timer);
        g_rec_timer = -1;
    }

    save_recording();
    free(g_rec_buf);
    g_rec_buf = NULL;
    g_rec_len = 0;
    g_rec_cap = 0;

    g_core->queue_tone(600, 200, 4000, KERCHUNK_PRI_LOW);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "voicemail", .description = "recording saved" } };
    kerchevt_fire(&ae);
}

/* Recording timer (max duration) */
static void rec_timeout(void *ud)
{
    (void)ud;
    g_rec_timer = -1;  /* Already fired */
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "recording max duration reached");
    stop_recording();
}

/* Audio tap callback for recording */
static void rec_audio_tap(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_recording || !evt->audio.samples)
        return;

    /* Cap buffer growth at max duration */
    size_t max_samples = (size_t)g_core->sample_rate * (size_t)g_max_duration_s;
    if (g_rec_len >= max_samples)
        return;

    /* Grow buffer if needed */
    size_t needed = g_rec_len + evt->audio.n;
    if (needed > g_rec_cap) {
        size_t new_cap = g_rec_cap * 2;
        if (new_cap < needed) new_cap = needed + g_core->sample_rate;
        if (new_cap > max_samples) new_cap = max_samples;
        int16_t *new_buf = realloc(g_rec_buf, new_cap * sizeof(int16_t));
        if (!new_buf) return;
        g_rec_buf = new_buf;
        g_rec_cap = new_cap;
    }

    size_t to_copy = evt->audio.n;
    if (g_rec_len + to_copy > max_samples)
        to_copy = max_samples - g_rec_len;
    memcpy(g_rec_buf + g_rec_len, evt->audio.samples, to_copy * sizeof(int16_t));
    g_rec_len += to_copy;
}

/* DTMF command handlers */

static void on_vm_status(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled || g_current_caller_id <= 0) return;

    int n = count_messages(g_current_caller_id);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "user %d has %d messages", g_current_caller_id, n);

    if (g_core->tts_speak) {
        char text[128];
        if (n == 0)
            snprintf(text, sizeof(text), "No voicemail messages.");
        else if (n == 1)
            snprintf(text, sizeof(text), "You have 1 voicemail message.");
        else
            snprintf(text, sizeof(text), "You have %d voicemail messages.", n);
        g_core->tts_speak(text, KERCHUNK_PRI_LOW);
    } else {
        /* Tone fallback: N beeps = N messages, low tone = none */
        for (int i = 0; i < n && i < 9; i++) {
            g_core->queue_tone(1000, 100, 4000, KERCHUNK_PRI_LOW);
            g_core->queue_silence(100, KERCHUNK_PRI_LOW);
        }
        if (n == 0)
            g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_LOW);
    }

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "voicemail", .description = "status" } };
    kerchevt_fire(&ae);
}

static void on_vm_record(const kerchevt_t *evt, void *ud)
{
    (void)ud;
    if (!g_enabled) return;

    /* Get target user from argument or use current caller */
    int target_id = g_current_caller_id;
    if (evt->custom.data && evt->custom.len > 1) {
        int arg = atoi((const char *)evt->custom.data);
        if (arg > 0) target_id = arg;
    }
    if (target_id <= 0) return;

    if (count_messages(target_id) >= g_max_messages) {
        g_core->queue_tone(400, 1000, 4000, KERCHUNK_PRI_LOW);  /* Error tone */
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "voicemail", .description = "mailbox full" } };
        kerchevt_fire(&ae);
        return;
    }

    /* Announce target */
    if (target_id != g_current_caller_id) {
        const kerchunk_user_t *target = g_core->user_lookup_by_id(target_id);
        if (target && g_core->tts_speak) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Recording message for %s.", target->name);
            g_core->tts_speak(msg, KERCHUNK_PRI_NORMAL);
        } else if (!target) {
            g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_LOW);  /* Error — user not found */
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "target user %d not found", target_id);
            return;
        }
    }

    /* Start recording */
    g_recording = 1;
    g_record_user_id = target_id;
    g_rec_len = 0;
    g_rec_cap = (size_t)g_core->sample_rate * (size_t)g_max_duration_s;
    g_rec_buf = malloc(g_rec_cap * sizeof(int16_t));
    if (!g_rec_buf) {
        g_recording = 0;
        return;
    }

    g_core->audio_tap_register(rec_audio_tap, NULL);
    g_rec_timer = g_core->timer_create(g_max_duration_s * 1000, 0, rec_timeout, NULL);
    if (g_rec_timer < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "failed to create recording timer");
        g_recording = 0;
        g_core->audio_tap_unregister(rec_audio_tap);
        free(g_rec_buf);
        g_rec_buf = NULL;
        return;
    }

    /* Beep to indicate recording started */
    g_core->queue_tone(800, 200, 4000, KERCHUNK_PRI_LOW);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "voicemail", .description = "recording started" } };
    kerchevt_fire(&ae);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "recording for user %d", target_id);
}

static void on_vm_play(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled || g_current_caller_id <= 0) return;

    /* Play first/next message */
    char path[512];
    snprintf(path, sizeof(path), "%s/%d", g_vm_dir, g_current_caller_id);
    DIR *d = opendir(path);
    if (!d) {
        g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_LOW);
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "voicemail", .description = "no messages" } };
        kerchevt_fire(&ae);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strstr(ent->d_name, ".pcm")) {
            char fpath[768];
            snprintf(fpath, sizeof(fpath), "%s/%s", path, ent->d_name);
            g_core->queue_audio_file(fpath, KERCHUNK_PRI_LOW);
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "playing: %s", fpath);

            kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
                .announcement = { .source = "voicemail", .description = "playback" } };
            kerchevt_fire(&ae);
            break;
        }
    }
    closedir(d);
}

static void on_vm_delete(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled || g_current_caller_id <= 0) return;

    char dirpath[512];
    snprintf(dirpath, sizeof(dirpath), "%s/%d", g_vm_dir, g_current_caller_id);
    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strstr(ent->d_name, ".pcm")) {
            char fpath[768];
            snprintf(fpath, sizeof(fpath), "%s/%s", dirpath, ent->d_name);
            remove(fpath);
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "deleted: %s", fpath);
            g_core->queue_tone(600, 200, 4000, KERCHUNK_PRI_LOW);  /* Confirm */

            kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
                .announcement = { .source = "voicemail", .description = "deleted" } };
            kerchevt_fire(&ae);
            break;
        }
    }
    closedir(d);
}

static void on_vm_list(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    /* Same as status for audio interface */
    kerchevt_t status_evt = { .type = (kerchevt_type_t)DTMF_EVT_VOICEMAIL_STATUS };
    on_vm_status(&status_evt, NULL);
}

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

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    /* Stop recording on COR drop (idempotent) */
    stop_recording();
}

static int voicemail_load(kerchunk_core_t *core)
{
    g_core = core;

    if (core->dtmf_register) {
        core->dtmf_register("87", 0, "Voicemail status", "voicemail_status");
        core->dtmf_register("86", 1, "Voicemail record", "voicemail_record");
        core->dtmf_register("85", 2, "Voicemail play",   "voicemail_play");
        core->dtmf_register("83", 3, "Voicemail delete", "voicemail_delete");
        core->dtmf_register("84", 4, "Voicemail list",   "voicemail_list");
    }

    core->subscribe(DTMF_EVT_VOICEMAIL_STATUS, on_vm_status, NULL);
    core->subscribe(DTMF_EVT_VOICEMAIL_RECORD, on_vm_record, NULL);
    core->subscribe(DTMF_EVT_VOICEMAIL_PLAY,   on_vm_play, NULL);
    core->subscribe(DTMF_EVT_VOICEMAIL_DELETE,  on_vm_delete, NULL);
    core->subscribe(DTMF_EVT_VOICEMAIL_LIST,    on_vm_list, NULL);
    core->subscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified, NULL);
    core->subscribe(KERCHEVT_CALLER_CLEARED, on_caller_cleared, NULL);
    core->subscribe(KERCHEVT_COR_DROP, on_cor_drop, NULL);

    return 0;
}

static int voicemail_configure(const kerchunk_config_t *cfg)
{
    const char *dir = kerchunk_config_get(cfg, "voicemail", "voicemail_dir");
    if (dir)
        snprintf(g_vm_dir, sizeof(g_vm_dir), "%s", dir);

    g_max_messages  = kerchunk_config_get_int(cfg, "voicemail", "max_messages", 20);
    g_max_duration_s = kerchunk_config_get_int(cfg, "voicemail", "max_duration", 60);
    if (g_max_duration_s > 300) g_max_duration_s = 300;  /* 5 min cap */
    if (g_max_duration_s < 1)   g_max_duration_s = 1;

    const char *en = kerchunk_config_get(cfg, "voicemail", "enabled");
    g_enabled = (en && strcmp(en, "on") == 0);

    if (mkdir(g_vm_dir, 0755) != 0 && errno != EEXIST)
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "cannot create directory: %s", g_vm_dir);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "voicemail dir=%s max_msg=%d max_dur=%ds enabled=%d",
                g_vm_dir, g_max_messages, g_max_duration_s, g_enabled);
    return 0;
}

static void voicemail_unload(void)
{
    if (g_core->dtmf_unregister) {
        g_core->dtmf_unregister("87");
        g_core->dtmf_unregister("86");
        g_core->dtmf_unregister("85");
        g_core->dtmf_unregister("83");
        g_core->dtmf_unregister("84");
    }

    g_core->unsubscribe(DTMF_EVT_VOICEMAIL_STATUS, on_vm_status);
    g_core->unsubscribe(DTMF_EVT_VOICEMAIL_RECORD, on_vm_record);
    g_core->unsubscribe(DTMF_EVT_VOICEMAIL_PLAY,   on_vm_play);
    g_core->unsubscribe(DTMF_EVT_VOICEMAIL_DELETE,  on_vm_delete);
    g_core->unsubscribe(DTMF_EVT_VOICEMAIL_LIST,    on_vm_list);
    g_core->unsubscribe(KERCHEVT_CALLER_IDENTIFIED, on_caller_identified);
    g_core->unsubscribe(KERCHEVT_CALLER_CLEARED, on_caller_cleared);
    g_core->unsubscribe(KERCHEVT_COR_DROP, on_cor_drop);

    if (g_rec_timer >= 0)
        g_core->timer_cancel(g_rec_timer);
    free(g_rec_buf);
    g_rec_buf = NULL;
}

/* CLI */
static int cli_voicemail(int argc, const char **argv, kerchunk_resp_t *r)
{
    if (argc >= 2 && strcmp(argv[1], "status") == 0) {
        resp_bool(r, "enabled", g_enabled);
        resp_str(r, "directory", g_vm_dir);
        /* JSON: users array */
        if (!r->jfirst) resp_json_raw(r, ",");
        resp_json_raw(r, "\"users\":[");
        int jfirst = 1;
        for (int i = 1; i <= 64; i++) {
            const kerchunk_user_t *u = g_core->user_lookup_by_id(i);
            if (u && u->voicemail) {
                int n = count_messages(i);
                if (!jfirst) resp_json_raw(r, ",");
                char frag[128];
                snprintf(frag, sizeof(frag),
                         "{\"name\":\"%s\",\"messages\":%d}", u->name, n);
                resp_json_raw(r, frag);
                jfirst = 0;
                /* Text */
                char line[64];
                snprintf(line, sizeof(line), "  %s: %d messages\n", u->name, n);
                resp_text_raw(r, line);
            }
        }
        resp_json_raw(r, "]");
        r->jfirst = 0;
    } else {
        goto usage;
    }
    return 0;

usage:
    resp_text_raw(r, "Voicemail system\n\n"
        "  voicemail status\n"
        "    Show voicemail status: enabled, directory, and per-user\n"
        "    message counts for all users with voicemail enabled.\n\n"
        "    DTMF commands (on-air):\n"
        "      *87#    Check voicemail status (message count)\n"
        "      *86#    Record a voicemail message\n"
        "      *85#    Play voicemail messages\n"
        "      *83#    Delete voicemail messages\n"
        "      *84#    List voicemail messages\n\n"
        "    Messages are stored as PCM files in per-user subdirectories.\n"
        "    Recording stops on COR drop or max duration timeout.\n\n"
        "Config: [voicemail] enabled, voicemail_dir, max_messages, max_duration\n");
    resp_str(r, "error", "usage: voicemail status");
    resp_finish(r);
    return -1;
}

static const kerchunk_cli_cmd_t cli_cmds[] = {
    { "voicemail", "voicemail status", "Voicemail status", cli_voicemail },
};

static kerchunk_module_def_t mod_voicemail = {
    .name         = "mod_voicemail",
    .version      = "1.0.0",
    .description  = "Voicemail system",
    .load         = voicemail_load,
    .configure    = voicemail_configure,
    .unload       = voicemail_unload,
    .cli_commands = cli_cmds,
    .num_cli_commands = 1,
};

KERCHUNK_MODULE_DEFINE(mod_voicemail);
