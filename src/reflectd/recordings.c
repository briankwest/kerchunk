/*
 * recordings.c — WAV writer + CSV CDR for kerchunk-reflectd. See header.
 *
 * The Opus decoder runs at the audio sample rate the link uses
 * (KERCHUNK_LINK_OPUS_SAMPLE_RATE = 24 kHz, mono). One decoder is created
 * per session and freed at the end — Opus decoders are cheap enough that
 * keeping a pool isn't worth the complexity.
 *
 * WAV format: canonical 44-byte RIFF/WAVE/fmt+data header written
 * up-front with placeholder sizes; closed by fseek-back and patching
 * the two size fields. PCM is 16-bit signed little-endian mono at 24
 * kHz — every audio tool plays it natively.
 */

#include "recordings.h"
#include "../../include/kerchunk_link_proto.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <opus/opus.h>

static char g_dir[256];
static int  g_enabled;
static int  g_max_age_days;   /* drives recordings_prune() */

struct rec_session {
    FILE        *wav;
    OpusDecoder *dec;
    uint32_t     pcm_bytes_written;
    uint16_t     tg;
    char         tg_name[64];
    char         node_id[64];
    char         rel_path[160];   /* YYYY-MM-DD/TG<n>_HHMMSS_<node>.wav */
    time_t       started_at;
};

const char *recordings_dir(void) { return g_dir; }

int recordings_global_init(const char *dir, int enabled, int max_age_days)
{
    snprintf(g_dir, sizeof(g_dir), "%s", dir ? dir : "");
    g_enabled = enabled ? 1 : 0;
    g_max_age_days = max_age_days;
    if (!g_enabled) return 0;
    if (!g_dir[0]) {
        fprintf(stderr, "recordings: enabled but recording_dir is empty\n");
        return -1;
    }
    /* Ensure the parent dir exists. We create per-day subdirs lazily as
     * the first session of each day opens its WAV. */
    if (mkdir(g_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "recordings: mkdir %s: %s\n", g_dir, strerror(errno));
        return -1;
    }
    return 0;
}

void recordings_global_shutdown(void) { /* no-op — sessions own their own state */ }

/* "YYYY-MM-DD" → days_since_epoch. Returns -1 on parse error. */
static int parse_ymd_to_days(const char *ymd)
{
    if (!ymd || strlen(ymd) != 10) return -1;
    if (ymd[4] != '-' || ymd[7] != '-') return -1;
    for (int i = 0; i < 10; i++)
        if (i != 4 && i != 7 && !isdigit((unsigned char)ymd[i])) return -1;
    struct tm tm = {0};
    tm.tm_year = atoi(ymd) - 1900;
    tm.tm_mon  = atoi(ymd + 5) - 1;
    tm.tm_mday = atoi(ymd + 8);
    /* Use timegm-equivalent: setenv TZ=UTC, mktime, restore. We just
     * use the seconds-since-epoch from mktime in local time and divide
     * by 86400 — fine for "older than N days" comparisons. */
    time_t t = mktime(&tm);
    if (t == (time_t)-1) return -1;
    return (int)(t / 86400);
}

/* rm -rf a directory recursively. */
static void rmrf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { unlink(path); return; }
    struct dirent *e;
    char child[512];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) rmrf(child);
        else                                                unlink(child);
    }
    closedir(d);
    rmdir(path);
}

int recordings_prune(void)
{
    if (!g_enabled || g_max_age_days <= 0 || !g_dir[0]) return 0;

    int today_days = (int)(time(NULL) / 86400);
    int cutoff = today_days - g_max_age_days;

    DIR *d = opendir(g_dir);
    if (!d) return 0;
    int removed = 0;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        /* Match either YYYY-MM-DD/ subdir (10 chars, no extension)
         * or YYYY-MM-DD.csv (14 chars). */
        size_t n = strlen(e->d_name);
        char ymd[11] = {0};
        if (n == 10) {
            memcpy(ymd, e->d_name, 10);
        } else if (n == 14 && strcmp(e->d_name + 10, ".csv") == 0) {
            memcpy(ymd, e->d_name, 10);
        } else {
            continue;
        }
        int day = parse_ymd_to_days(ymd);
        if (day < 0 || day >= cutoff) continue;

        snprintf(path, sizeof(path), "%s/%s", g_dir, e->d_name);
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) rmrf(path);
        else                                               unlink(path);
        removed++;
    }
    closedir(d);
    return removed;
}

/* Write the canonical 44-byte WAV header. Sizes are patched in
 * recordings_end once we know how many PCM bytes were appended. */
static void wav_write_header(FILE *fp, int sample_rate)
{
    static const uint8_t hdr[44] = {0};   /* placeholder */
    fwrite(hdr, 1, sizeof(hdr), fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t h[44];
    memcpy(h + 0,  "RIFF", 4);
    /* file size - 8: patched on close */
    h[4] = h[5] = h[6] = h[7] = 0;
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    h[16] = 16; h[17] = 0; h[18] = 0; h[19] = 0;     /* fmt chunk size */
    h[20] = 1;  h[21] = 0;                            /* PCM */
    h[22] = 1;  h[23] = 0;                            /* mono */
    /* sample rate (LE) */
    h[24] = (uint8_t)(sample_rate);
    h[25] = (uint8_t)(sample_rate >> 8);
    h[26] = (uint8_t)(sample_rate >> 16);
    h[27] = (uint8_t)(sample_rate >> 24);
    /* byte rate = sample_rate * 2 */
    uint32_t br = sample_rate * 2;
    h[28] = (uint8_t)(br); h[29] = (uint8_t)(br >> 8);
    h[30] = (uint8_t)(br >> 16); h[31] = (uint8_t)(br >> 24);
    h[32] = 2;  h[33] = 0;                            /* block align */
    h[34] = 16; h[35] = 0;                            /* bits per sample */
    memcpy(h + 36, "data", 4);
    /* data chunk size: patched on close */
    h[40] = h[41] = h[42] = h[43] = 0;
    fwrite(h, 1, sizeof(h), fp);
}

static void wav_finalize(FILE *fp, uint32_t pcm_bytes)
{
    /* RIFF size at offset 4 = 36 + pcm_bytes */
    uint32_t riff = 36 + pcm_bytes;
    fseek(fp, 4, SEEK_SET);
    fwrite(&riff, 4, 1, fp);
    /* data size at offset 40 = pcm_bytes */
    fseek(fp, 40, SEEK_SET);
    fwrite(&pcm_bytes, 4, 1, fp);
    fseek(fp, 0, SEEK_END);
}

rec_session_t *recordings_start(uint16_t tg, const char *tg_name,
                                const char *node_id)
{
    if (!g_enabled) return NULL;

    time_t now = time(NULL);
    struct tm tb;
    localtime_r(&now, &tb);

    /* Per-day subdir. */
    char day_dir[300];
    snprintf(day_dir, sizeof(day_dir), "%s/%04d-%02d-%02d",
             g_dir, tb.tm_year + 1900, tb.tm_mon + 1, tb.tm_mday);
    if (mkdir(day_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "recordings: mkdir %s: %s\n", day_dir, strerror(errno));
        return NULL;
    }

    /* Sanitize node_id for filesystem (drop anything not alphanumeric,
     * dash, underscore). The roster ids are usually clean callsigns, but
     * a malformed one shouldn't escape the day dir. */
    char safe_node[64]; size_t si = 0;
    for (size_t i = 0; node_id && node_id[i] && si < sizeof(safe_node) - 1; i++) {
        char c = node_id[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_')
            safe_node[si++] = c;
    }
    safe_node[si] = '\0';
    if (!si) snprintf(safe_node, sizeof(safe_node), "unknown");

    rec_session_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->tg         = tg;
    s->started_at = now;
    snprintf(s->tg_name, sizeof(s->tg_name), "%s", tg_name ? tg_name : "");
    snprintf(s->node_id, sizeof(s->node_id), "%s", node_id ? node_id : "");
    snprintf(s->rel_path, sizeof(s->rel_path),
             "%04d-%02d-%02d/TG%u_%02d%02d%02d_%s.wav",
             tb.tm_year + 1900, tb.tm_mon + 1, tb.tm_mday, tg,
             tb.tm_hour, tb.tm_min, tb.tm_sec, safe_node);

    char abs_path[512];
    snprintf(abs_path, sizeof(abs_path), "%s/%s", g_dir, s->rel_path);
    s->wav = fopen(abs_path, "wb+");
    if (!s->wav) {
        fprintf(stderr, "recordings: open %s: %s\n", abs_path, strerror(errno));
        free(s);
        return NULL;
    }
    wav_write_header(s->wav, KERCHUNK_LINK_OPUS_SAMPLE_RATE);

    int err = 0;
    s->dec = opus_decoder_create(KERCHUNK_LINK_OPUS_SAMPLE_RATE, 1, &err);
    if (!s->dec) {
        fprintf(stderr, "recordings: opus_decoder_create %d\n", err);
        fclose(s->wav);
        unlink(abs_path);
        free(s);
        return NULL;
    }
    return s;
}

void recordings_append(rec_session_t *s, const uint8_t *opus, int opus_len)
{
    if (!s || !s->wav || !s->dec || opus_len <= 0) return;
    int16_t pcm[KERCHUNK_LINK_OPUS_FRAME_SAMPLES];
    int n = opus_decode(s->dec, opus, opus_len, pcm,
                        KERCHUNK_LINK_OPUS_FRAME_SAMPLES, 0);
    if (n <= 0) return;
    size_t bytes = (size_t)n * 2;
    if (fwrite(pcm, 1, bytes, s->wav) == bytes)
        s->pcm_bytes_written += (uint32_t)bytes;
}

/* RFC 4180 minimal CSV escaping: wrap in quotes if the field contains a
 * comma, quote, or newline; double up any embedded quotes. */
static void csv_write_field(FILE *fp, const char *s)
{
    if (!s) { return; }
    int needs_quote = 0;
    for (const char *p = s; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r')
            { needs_quote = 1; break; }
    if (!needs_quote) { fputs(s, fp); return; }
    fputc('"', fp);
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputc('"', fp);
        fputc(*p, fp);
    }
    fputc('"', fp);
}

void recordings_end(rec_session_t *s)
{
    if (!s) return;
    if (s->wav) {
        wav_finalize(s->wav, s->pcm_bytes_written);
        fclose(s->wav);
    }
    if (s->dec) opus_decoder_destroy(s->dec);

    /* Append to today's CSV. Same day as the WAV — derive from started_at
     * so a session that crosses midnight still lands in its start day. */
    if (g_enabled && g_dir[0]) {
        struct tm tb;
        localtime_r(&s->started_at, &tb);
        char csv_path[300];
        snprintf(csv_path, sizeof(csv_path), "%s/%04d-%02d-%02d.csv",
                 g_dir, tb.tm_year + 1900, tb.tm_mon + 1, tb.tm_mday);
        int new_file = (access(csv_path, F_OK) != 0);
        FILE *fp = fopen(csv_path, "a");
        if (fp) {
            if (new_file) {
                fputs("timestamp,date,time,tg,tg_name,node_id,"
                      "duration_s,pcm_bytes,recording\n", fp);
            }
            float dur = (s->pcm_bytes_written / 2) /
                        (float)KERCHUNK_LINK_OPUS_SAMPLE_RATE;
            fprintf(fp, "%ld,%04d-%02d-%02d,%02d:%02d:%02d,%u,",
                    (long)s->started_at,
                    tb.tm_year + 1900, tb.tm_mon + 1, tb.tm_mday,
                    tb.tm_hour, tb.tm_min, tb.tm_sec, s->tg);
            csv_write_field(fp, s->tg_name);
            fputc(',', fp);
            csv_write_field(fp, s->node_id);
            fprintf(fp, ",%.2f,%u,", dur, s->pcm_bytes_written);
            csv_write_field(fp, s->rel_path);
            fputc('\n', fp);
            fclose(fp);
        }
    }
    free(s);
}
