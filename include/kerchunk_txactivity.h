/*
 * kerchunk_txactivity.h — Fused TX-activity detector
 *
 * Decides "user is transmitting" by OR'ing two independent inputs:
 *   * raw HID COS bit (sticky over -1 = no-data returns)
 *   * libplcode DTMF decoder's detected state (true while a tone is locked)
 *
 * Either input asserts presence; both must be quiet continuously for
 * end_silence_ticks before TX_END is declared.
 *
 * Adaptive: when DTMF activity has been seen within dtmf_grace_ticks,
 * the silence requirement extends to end_silence_dtmf_ticks so
 * inter-tone gaps on tight-squelch radios (Retevis) don't cause
 * premature TX_END.
 *
 * Designed as a pure function over a state struct so it's unit-testable
 * without any of kerchunkd's runtime (no atomics, no event bus, no
 * config — caller provides per-tick inputs and dispatches the returned
 * edge event).
 *
 * See ARCH-COR-DTMF.md for the design rationale.
 */

#ifndef KERCHUNK_TXACTIVITY_H
#define KERCHUNK_TXACTIVITY_H

typedef enum {
    KERCHUNK_TXACT_NONE  = 0,   /* no edge this tick */
    KERCHUNK_TXACT_BEGIN = 1,   /* fire KERCHEVT_COR_ASSERT */
    KERCHUNK_TXACT_END   = 2,   /* fire KERCHEVT_COR_DROP */
} kerchunk_txact_event_t;

typedef struct {
    /* Configuration — set by init() */
    int end_silence_ticks;       /* normal voice-mode silence requirement */
    int end_silence_dtmf_ticks;  /* extended silence within dtmf_grace */
    int dtmf_grace_ticks;        /* how long after last DTMF we stay patient */
    int trust_cos_bit;           /* 1 = consider cos_bit; 0 = ignore */

    /* Mutable state */
    int published;               /* edge state we last reported (0/1) */
    int silent_ticks;            /* consecutive ticks with all inputs quiet */
    int dtmf_seen_ago;           /* ticks since last dtmf_active=1 */
    int cos_bit_sticky;          /* 0/1 — last definitive cos value, sticky over -1 */
    int cos_flapped_session;     /* 0/1 — set when cos_bit went 0→1 during an
                                  * active session (i.e. it had previously
                                  * dropped mid-keyup and came back up).
                                  * Radios whose CTCSS/DCS decoder loses
                                  * lock during voice pauses (Retevis) flap
                                  * reliably; this flag pins the detector
                                  * into the longer dtmf-patient silence
                                  * window for the rest of the session so
                                  * the next brief COS drop doesn't trigger
                                  * a premature TX_END. Cleared on TX_END. */
} kerchunk_txactivity_t;

/* Initialize state. All ticks values are in 20 ms units (caller has
 * already converted from ms). end_silence_dtmf_ticks is clamped to be
 * at least end_silence_ticks. */
void kerchunk_txactivity_init(kerchunk_txactivity_t *s,
                              int end_silence_ticks,
                              int end_silence_dtmf_ticks,
                              int dtmf_grace_ticks,
                              int trust_cos_bit);

/* Per-tick update. Returns the edge event the caller should fire.
 *   cos_raw       — kerchunk_hid_read_cor() result: 0, 1, or -1 (no data).
 *                   When -1, the previous sticky cos value is reused.
 *   dtmf_active   — 0 or 1: libplcode decoder.detected for this tick.
 * The caller should call set_cor() and fire the matching event when
 * BEGIN or END is returned. NONE means no edge crossed this tick. */
kerchunk_txact_event_t
kerchunk_txactivity_tick(kerchunk_txactivity_t *s,
                         int cos_raw,
                         int dtmf_active);

/* Compute which silence threshold is in effect this tick (ticks units).
 * Exposed for diagnostic / log purposes. */
int kerchunk_txactivity_active_silence_ticks(const kerchunk_txactivity_t *s);

#endif /* KERCHUNK_TXACTIVITY_H */
