/*
 * recordings.h — per-call WAV recording + CSV CDR for kerchunk-reflectd.
 *
 * One "call" = one floor session = the span between floor IDLE→TALKING(N)
 * and the eventual floor release. We open a WAV file at session start,
 * decode each authenticated Opus packet (the cleartext payload that the
 * reflector is about to fan out) into 24 kHz mono int16, append it, and
 * close + emit a CDR row when the floor releases.
 *
 * File layout under recording_dir:
 *
 *   YYYY-MM-DD/TG<n>_HHMMSS_<node>.wav    ← per-call WAV
 *   YYYY-MM-DD.csv                         ← per-day CDR (one row per call)
 *
 * Recording is opt-in: recordings_global_init returns 0 with no-op
 * sessions when recording_enabled is off.
 */

#ifndef KERCHUNK_REFLECTD_RECORDINGS_H
#define KERCHUNK_REFLECTD_RECORDINGS_H

#include <stdint.h>

typedef struct rec_session rec_session_t;

int  recordings_global_init(const char *dir, int enabled, int max_age_days);
void recordings_global_shutdown(void);

/* Returns NULL if recording is disabled, the dir doesn't exist, or the
 * file can't be opened. Callers must tolerate NULL transparently. */
rec_session_t *recordings_start(uint16_t tg, const char *tg_name,
                                const char *node_id);
void           recordings_append(rec_session_t *s,
                                 const uint8_t *opus, int opus_len);
void           recordings_end(rec_session_t *s);

/* Path helpers used by the HTTP API. Caller-allocated buf. */
const char *recordings_dir(void);

#endif /* KERCHUNK_REFLECTD_RECORDINGS_H */
