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
#include <stdatomic.h>
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

/* Recording state (g_recording read by audio thread tap).
 *
 * Two-phase flow:
 *   1. *86<id># dispatched on COR drop -> on_vm_record arms the recorder
 *      (sets g_record_armed and target). No tap, no timer yet.
 *   2. Next COR_ASSERT -> on_cor_assert starts the tap and timer, clears
 *      armed flag, sets g_recording.
 *   3. Next COR_DROP -> on_cor_drop -> stop_recording saves the buffer.
 *
 * The arm step exists to avoid the race where mod_dtmfcmd's deferred
 * dispatch and mod_voicemail's COR_DROP handler both fire on the *same*
 * COR_DROP event — without arming, recording would start and immediately
 * be saved (empty) on that single COR cycle.
 */
static atomic_int g_recording;
static int      g_record_armed;
static int      g_record_user_id;
static int16_t *g_rec_buf;
static size_t   g_rec_len;
static size_t   g_rec_cap;
static int      g_rec_timer = -1;
static int      g_arm_timer = -1;        /* expires the armed state if user never keys up */
static int      g_arm_timeout_ms = 30000; /* 30s to start recording after dial */

/* Last caller seen on the channel. Persists across COR drops because the
 * voicemail commands are dispatched on COR drop — and mod_caller fires
 * CALLER_CLEARED on the same COR_DROP event, racing us. Tracking the last
 * identified user (instead of the transient "actively transmitting" id)
 * sidesteps the race and matches the login session lifetime. */
static int g_current_caller_id;

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

/* Authoritative voicemail-state snapshot for admin dashboard.
 * Sensitive (per-user message counts, names) → admin_only=1.
 * Called on every state transition: armed/disarmed, recording
 * start/stop, message saved, message deleted. Also seeded at
 * configure() so the SSE cache always carries truth. */
static void publish_voicemail_snapshot(void)
{
    if (!g_core || !g_core->sse_publish) return;

    char users[2048];
    size_t up = 0;
    int total = 0;
    int armed_uid     = g_record_armed ? g_record_user_id : 0;
    int recording_uid = g_recording    ? g_record_user_id : 0;

    users[0] = '\0';
    int first = 1;
    for (int i = 1; i <= 64; i++) {
        const kerchunk_user_t *u = g_core->user_lookup_by_id(i);
        if (!u || !u->voicemail) continue;
        int n = count_messages(i);
        total += n;

        char e_name[64];
        size_t j = 0;
        for (const char *p = u->name; *p && j < sizeof(e_name) - 6; p++) {
            switch (*p) {
            case '"':  e_name[j++] = '\\'; e_name[j++] = '"';  break;
            case '\\': e_name[j++] = '\\'; e_name[j++] = '\\'; break;
            default:   e_name[j++] = *p;                       break;
            }
        }
        e_name[j] = '\0';

        char frag[200];
        int flen = snprintf(frag, sizeof(frag),
            "%s{\"user_id\":%d,\"name\":\"%s\",\"count\":%d,\"full\":%s}",
            first ? "" : ",",
            u->id, e_name, n, (n >= g_max_messages) ? "true" : "false");
        if (flen < 0) continue;
        if (up + (size_t)flen >= sizeof(users)) break;
        memcpy(users + up, frag, (size_t)flen);
        up += (size_t)flen;
        users[up] = '\0';
        first = 0;
    }

    char json[3072];
    snprintf(json, sizeof(json),
        "{\"enabled\":%s,"
        "\"armed\":%s,"
        "\"armed_user_id\":%d,"
        "\"recording\":%s,"
        "\"recording_user_id\":%d,"
        "\"max_messages\":%d,"
        "\"max_duration_s\":%d,"
        "\"total_messages\":%d,"
        "\"users\":[%s]}",
        g_enabled            ? "true" : "false",
        g_record_armed       ? "true" : "false",
        armed_uid,
        g_recording          ? "true" : "false",
        recording_uid,
        g_max_messages,
        g_max_duration_s,
        total,
        users);

    g_core->sse_publish("voicemail_updated", json, /*admin_only=*/1);
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
static void arm_timeout(void *ud);

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

    publish_voicemail_snapshot();
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

/* Audible reject — speaks the reason via TTS when available so the user
 * understands why the command did nothing, with a tone-pair fallback for
 * setups without TTS. Always logs at INFO so operators can debug. */
static void reject(const char *spoken_reason)
{
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "rejected: %s", spoken_reason);
    if (g_core->tts_speak) {
        g_core->tts_speak(spoken_reason, KERCHUNK_PRI_LOW);
    } else {
        g_core->queue_tone(400, 200, 4000, KERCHUNK_PRI_LOW);
        g_core->queue_silence(80, KERCHUNK_PRI_LOW);
        g_core->queue_tone(300, 200, 4000, KERCHUNK_PRI_LOW);
    }
}

static void on_vm_status(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) { reject("Voicemail is disabled."); return; }
    if (g_current_caller_id <= 0) {
        reject("Please log in before using voicemail.");
        return;
    }

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
    if (!g_enabled) { reject("Voicemail is disabled."); return; }

    /* Get target user from argument or use current caller */
    int target_id = g_current_caller_id;
    if (evt->custom.data && evt->custom.len > 1) {
        int arg = atoi((const char *)evt->custom.data);
        if (arg > 0) target_id = arg;
    }
    if (target_id <= 0) {
        reject("Please log in before recording a voicemail.");
        return;
    }

    if (count_messages(target_id) >= g_max_messages) {
        reject("Mailbox is full.");
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
            reject("Unknown user.");
            g_core->log(KERCHUNK_LOG_WARN, LOG_MOD, "target user %d not found", target_id);
            return;
        }
    }

    /* Arm the recorder — actual capture starts on the next COR assert.
     * See header comment on g_record_armed for the two-phase flow. */
    g_record_armed = 1;
    g_record_user_id = target_id;

    if (g_arm_timer >= 0) {
        g_core->timer_cancel(g_arm_timer);
        g_arm_timer = -1;
    }
    g_arm_timer = g_core->timer_create(g_arm_timeout_ms, 0, arm_timeout, NULL);

    /* Prompt the user — TTS if available, otherwise a tone */
    if (g_core->tts_speak) {
        char msg[160];
        if (target_id == g_current_caller_id) {
            snprintf(msg, sizeof(msg),
                     "Begin speaking after the beep. Unkey when done.");
        } else {
            const kerchunk_user_t *target = g_core->user_lookup_by_id(target_id);
            snprintf(msg, sizeof(msg),
                     "Recording message for %s. Begin after the beep, unkey when done.",
                     target ? target->name : "user");
        }
        g_core->tts_speak(msg, KERCHUNK_PRI_NORMAL);
    }
    g_core->queue_tone(800, 200, 4000, KERCHUNK_PRI_LOW);

    kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
        .announcement = { .source = "voicemail", .description = "recording armed" } };
    kerchevt_fire(&ae);

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "recorder armed for user %d (timeout %ds)",
                target_id, g_arm_timeout_ms / 1000);

    publish_voicemail_snapshot();
}

/* Recorder was armed but the user never keyed up to speak — disarm. */
static void arm_timeout(void *ud)
{
    (void)ud;
    g_arm_timer = -1;
    if (g_record_armed) {
        g_record_armed = 0;
        g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                    "recorder disarmed: no key-up within %ds",
                    g_arm_timeout_ms / 1000);
        if (g_core->tts_speak)
            g_core->tts_speak("Voicemail timed out.", KERCHUNK_PRI_LOW);
        publish_voicemail_snapshot();
    }
}

/* Begin actual capture — called from on_cor_assert when armed. */
static void start_recording_now(void)
{
    if (g_arm_timer >= 0) {
        g_core->timer_cancel(g_arm_timer);
        g_arm_timer = -1;
    }
    g_record_armed = 0;

    g_rec_len = 0;
    g_rec_cap = (size_t)g_core->sample_rate * (size_t)g_max_duration_s;
    g_rec_buf = malloc(g_rec_cap * sizeof(int16_t));
    if (!g_rec_buf) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "rec_buf alloc failed");
        return;
    }

    g_recording = 1;
    g_core->audio_tap_register(rec_audio_tap, NULL);
    g_rec_timer = g_core->timer_create(g_max_duration_s * 1000, 0,
                                       rec_timeout, NULL);
    if (g_rec_timer < 0) {
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD, "rec timer create failed");
        g_recording = 0;
        g_core->audio_tap_unregister(rec_audio_tap);
        free(g_rec_buf);
        g_rec_buf = NULL;
        return;
    }

    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD,
                "recording for user %d", g_record_user_id);

    publish_voicemail_snapshot();
}

static void on_vm_play(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) { reject("Voicemail is disabled."); return; }
    if (g_current_caller_id <= 0) {
        reject("Please log in before using voicemail.");
        return;
    }

    /* Play first/next message */
    char path[512];
    snprintf(path, sizeof(path), "%s/%d", g_vm_dir, g_current_caller_id);
    DIR *d = opendir(path);
    if (!d) {
        if (g_core->tts_speak)
            g_core->tts_speak("You have no voicemail messages.", KERCHUNK_PRI_LOW);
        else
            g_core->queue_tone(400, 500, 4000, KERCHUNK_PRI_LOW);
        kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
            .announcement = { .source = "voicemail", .description = "no messages" } };
        kerchevt_fire(&ae);
        return;
    }

    int played = 0;
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
            played = 1;
            break;
        }
    }
    closedir(d);
    if (!played && g_core->tts_speak)
        g_core->tts_speak("You have no voicemail messages.", KERCHUNK_PRI_LOW);
}

static void on_vm_delete(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (!g_enabled) { reject("Voicemail is disabled."); return; }
    if (g_current_caller_id <= 0) {
        reject("Please log in before using voicemail.");
        return;
    }

    char dirpath[512];
    snprintf(dirpath, sizeof(dirpath), "%s/%d", g_vm_dir, g_current_caller_id);
    DIR *d = opendir(dirpath);
    if (!d) {
        if (g_core->tts_speak)
            g_core->tts_speak("You have no voicemail messages to delete.",
                              KERCHUNK_PRI_LOW);
        return;
    }

    int deleted = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strstr(ent->d_name, ".pcm")) {
            char fpath[768];
            snprintf(fpath, sizeof(fpath), "%s/%s", dirpath, ent->d_name);
            remove(fpath);
            g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "deleted: %s", fpath);
            if (g_core->tts_speak)
                g_core->tts_speak("Message deleted.", KERCHUNK_PRI_LOW);
            else
                g_core->queue_tone(600, 200, 4000, KERCHUNK_PRI_LOW);

            kerchevt_t ae = { .type = KERCHEVT_ANNOUNCEMENT,
                .announcement = { .source = "voicemail", .description = "deleted" } };
            kerchevt_fire(&ae);
            deleted = 1;
            break;
        }
    }
    closedir(d);
    if (!deleted && g_core->tts_speak)
        g_core->tts_speak("You have no voicemail messages to delete.",
                          KERCHUNK_PRI_LOW);
    if (deleted)
        publish_voicemail_snapshot();
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

/* Note: we deliberately do NOT subscribe to KERCHEVT_CALLER_CLEARED.
 * mod_caller fires CALLER_CLEARED on every COR drop for login sessions,
 * even though the session itself persists. Clearing g_current_caller_id
 * here would race with the deferred DTMF command dispatch (also on
 * COR_DROP) and cause every voicemail command to silently no-op. */

/* Begin recording on COR assert if armed.
 * The COR_ASSERT we want is the user's *next* key-up after dialing
 * *86<id># — not the same key cycle that delivered the digits, because
 * the dial transmission has already ended (we're waiting in the armed
 * state). on_vm_record clears the armed flag after starting capture
 * so a held PTT mid-recording doesn't restart the buffer. */
static void on_cor_assert(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    if (g_record_armed && !g_recording)
        start_recording_now();
}

static void on_cor_drop(const kerchevt_t *evt, void *ud)
{
    (void)evt; (void)ud;
    /* Stop recording on COR drop (idempotent — only acts if g_recording).
     * The dial transmission's COR drop is harmless because g_recording is
     * still 0 at that point — we're only armed, not recording. */
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
    core->subscribe(KERCHEVT_COR_ASSERT, on_cor_assert, NULL);
    core->subscribe(KERCHEVT_COR_DROP, on_cor_drop, NULL);

    return 0;
}

static int voicemail_configure(const kerchunk_config_t *cfg)
{
    const char *dir = kerchunk_config_get(cfg, "voicemail", "voicemail_dir");
    if (dir)
        snprintf(g_vm_dir, sizeof(g_vm_dir), "%s", dir);

    g_max_messages  = kerchunk_config_get_int(cfg, "voicemail", "max_messages", 20);
    g_max_duration_s = kerchunk_config_get_duration_s(cfg, "voicemail", "max_duration", 60);
    if (g_max_duration_s > 300) g_max_duration_s = 300;  /* 5 min cap */
    if (g_max_duration_s < 1)   g_max_duration_s = 1;

    const char *en = kerchunk_config_get(cfg, "voicemail", "enabled");
    g_enabled = (en && strcmp(en, "on") == 0);

    if (mkdir(g_vm_dir, 0755) != 0 && errno != EEXIST)
        g_core->log(KERCHUNK_LOG_ERROR, LOG_MOD,
                    "cannot create directory: %s", g_vm_dir);
    g_core->log(KERCHUNK_LOG_INFO, LOG_MOD, "voicemail dir=%s max_msg=%d max_dur=%ds enabled=%d",
                g_vm_dir, g_max_messages, g_max_duration_s, g_enabled);

    publish_voicemail_snapshot();
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
    g_core->unsubscribe(KERCHEVT_COR_ASSERT, on_cor_assert);
    g_core->unsubscribe(KERCHEVT_COR_DROP, on_cor_drop);

    if (g_arm_timer >= 0)
        g_core->timer_cancel(g_arm_timer);
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
                /* Escape name for safe JSON embedding */
                char e_name[64];
                {
                    size_t j = 0;
                    for (const char *p = u->name; *p && j < sizeof(e_name) - 6; p++) {
                        switch (*p) {
                        case '"':  e_name[j++] = '\\'; e_name[j++] = '"'; break;
                        case '\\': e_name[j++] = '\\'; e_name[j++] = '\\'; break;
                        default:   e_name[j++] = *p; break;
                        }
                    }
                    e_name[j] = '\0';
                }
                char frag[128];
                snprintf(frag, sizeof(frag),
                         "{\"name\":\"%s\",\"messages\":%d}", e_name, n);
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
    { .name = "voicemail", .usage = "voicemail status", .description = "Voicemail status", .handler = cli_voicemail, .category = "Audio",
      .subcommands = "status" },
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
