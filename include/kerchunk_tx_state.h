/*
 * kerchunk_tx_state.h — TX state machine
 *
 * A pure function over a small input struct that returns the current
 * TX state label. Called each audio-thread tick; caller tracks the
 * previous value and fires KERCHEVT_TX_STATE_CHANGE on change.
 *
 * Six states covering the full TX lifecycle:
 *
 *   IDLE   — nothing transmitting
 *   RELAY  — software-relay mode, retransmitting live user audio
 *   DELAY  — queue-driven TX: PTT asserted, feeding tx_delay silence
 *   DRAIN  — queue-driven TX: feeding queued audio (TTS / WAV / tones)
 *   TAIL   — feeding tx_tail silence (queue) OR relay_drain silence
 *   HOLD   — queue released its PTT ref; core refcount still > 0
 *            while ptt_hold_ticks waits for PA to flush the ring
 *
 * Normal progressions:
 *
 *   Queue TX:   IDLE ─▶ DELAY ─▶ DRAIN ─▶ TAIL ─▶ HOLD ─▶ IDLE
 *   Relay TX:   IDLE ─▶ RELAY ─▶ TAIL ─▶ IDLE
 *   Mid-relay requeue: RELAY ─▶ TAIL ─▶ DELAY ─▶ DRAIN ─▶ ...
 *
 * Design notes:
 *
 * * RELAY hysteresis is inherited from the txactivity layer — the
 *   cor_active input already reflects the fused-detector's
 *   end-silence window, which extends to 1000 ms whenever
 *   cos_flapped_session is set. A Retevis CTCSS chop never reaches
 *   this FSM as a cor_active transition; it's already absorbed.
 *
 * * RELAY takes precedence over queue-driven states. If COR is
 *   active with software_relay=on, the live user audio wins —
 *   queue-pause guard in the TX sub-tick prevents the queue from
 *   asserting PTT during RELAY anyway.
 *
 * * Pure function, no state struct, no allocation, no locking.
 *   Safe to call from any thread, though in practice only the
 *   audio thread does. Cross-thread reads of the current label
 *   go through a caller-owned _Atomic cache (see src/main.c).
 *
 * Companion plan: PLAN-STATE-MODEL.md Phase 1.
 */

#ifndef KERCHUNK_TX_STATE_H
#define KERCHUNK_TX_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KERCHUNK_TX_IDLE  = 0,
    KERCHUNK_TX_RELAY = 1,
    KERCHUNK_TX_DELAY = 2,
    KERCHUNK_TX_DRAIN = 3,
    KERCHUNK_TX_TAIL  = 4,
    KERCHUNK_TX_HOLD  = 5,
} kerchunk_tx_state_t;

/* Wire-level label (used for logs, JSON, UI data attributes).
 * Never NULL; returns "TX_UNKNOWN" for out-of-range input. */
const char *kerchunk_tx_state_name(kerchunk_tx_state_t s);

/* Inputs to the FSM for a single tick. All of these are already
 * sampled in the audio thread each tick; gathering them into a
 * struct makes the pure function unit-testable without dragging
 * in kerchunk_core / g_audio_state dependencies. */
typedef struct {
    int cor_active;       /* is_receiving() — post-txactivity */
    int software_relay;   /* [repeater] software_relay == "on" */
    int queue_ptt;        /* g_audio_state.queue_ptt */
    int tx_delay_rem;     /* g_audio_state.tx_delay_rem */
    int tx_tail_rem;      /* g_audio_state.tx_tail_rem; -1 = idle */
    int relay_drain;      /* g_audio_state.relay_drain (samples) */
    int ptt_held;         /* kerchunk_core_get_ptt() — core refcount>0 */
} kerchunk_tx_state_inputs_t;

/* Pure compute of current state from inputs. Caller diffs against
 * previous tick's value to detect transitions. */
kerchunk_tx_state_t kerchunk_tx_state_compute(
    const kerchunk_tx_state_inputs_t *in);

#ifdef __cplusplus
}
#endif

#endif /* KERCHUNK_TX_STATE_H */
