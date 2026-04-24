/*
 * kerchunk_txactivity.c — implementation of the fused TX-activity detector.
 * See kerchunk_txactivity.h for the API contract and ARCH-COR-DTMF.md
 * for the design rationale.
 */

#include "kerchunk_txactivity.h"
#include <limits.h>
#include <string.h>

void kerchunk_txactivity_init(kerchunk_txactivity_t *s,
                              int end_silence_ticks,
                              int end_silence_dtmf_ticks,
                              int dtmf_grace_ticks,
                              int trust_cos_bit)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (end_silence_ticks < 0) end_silence_ticks = 0;
    if (end_silence_dtmf_ticks < end_silence_ticks)
        end_silence_dtmf_ticks = end_silence_ticks;
    if (dtmf_grace_ticks < 0) dtmf_grace_ticks = 0;

    s->end_silence_ticks      = end_silence_ticks;
    s->end_silence_dtmf_ticks = end_silence_dtmf_ticks;
    s->dtmf_grace_ticks       = dtmf_grace_ticks;
    s->trust_cos_bit          = trust_cos_bit ? 1 : 0;

    s->dtmf_seen_ago = INT_MAX / 2;  /* "very long ago" — voice mode at start */
}

int kerchunk_txactivity_active_silence_ticks(const kerchunk_txactivity_t *s)
{
    if (!s) return 0;
    return (s->dtmf_seen_ago < s->dtmf_grace_ticks)
        ? s->end_silence_dtmf_ticks
        : s->end_silence_ticks;
}

kerchunk_txact_event_t
kerchunk_txactivity_tick(kerchunk_txactivity_t *s,
                         int cos_raw,
                         int dtmf_active)
{
    if (!s) return KERCHUNK_TXACT_NONE;

    /* COS bit handling — sticky over HID -1 (no-data) returns. Only
     * accept definitive 0 or 1 readings. trust_cos_bit=0 forces
     * cos_bit to 0 regardless of input. */
    if (s->trust_cos_bit) {
        if (cos_raw == 0 || cos_raw == 1)
            s->cos_bit_sticky = cos_raw;
    } else {
        s->cos_bit_sticky = 0;
    }
    int cos_bit = s->cos_bit_sticky;

    /* Track ticks since last DTMF tone, for the adaptive end-silence. */
    if (dtmf_active) {
        s->dtmf_seen_ago = 0;
    } else if (s->dtmf_seen_ago < INT_MAX / 2) {
        s->dtmf_seen_ago++;
    }

    int present_now = cos_bit || dtmf_active;
    int active_silence = kerchunk_txactivity_active_silence_ticks(s);

    if (present_now) {
        s->silent_ticks = 0;
        if (!s->published) {
            s->published = 1;
            return KERCHUNK_TXACT_BEGIN;
        }
        return KERCHUNK_TXACT_NONE;
    }

    /* present_now == 0 */
    if (s->published) {
        s->silent_ticks++;
        if (s->silent_ticks >= active_silence) {
            s->published = 0;
            s->silent_ticks = 0;
            return KERCHUNK_TXACT_END;
        }
    }
    return KERCHUNK_TXACT_NONE;
}
