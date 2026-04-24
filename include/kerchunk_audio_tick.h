/*
 * kerchunk_audio_tick.h — State + (eventually) pure-function API for
 * the audio thread.
 *
 * Phase 0 of the plan in PLAN-AUDIO-TICK.md: consolidate the audio-
 * thread-owned file-scope globals in src/main.c into one struct. No
 * behavioral change; this just reshapes the declarations so future
 * phases can pass the state to a pure kerchunk_audio_tick() function.
 *
 * Later phases add kerchunk_audio_input_t / kerchunk_audio_output_t /
 * kerchunk_audio_tick() to this header. They are intentionally NOT
 * declared yet — empty API is better than a half-built one.
 */

#ifndef KERCHUNK_AUDIO_TICK_H
#define KERCHUNK_AUDIO_TICK_H

#include <stdint.h>
#include <stddef.h>

#include "plcode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Audio-thread state. Mutated exclusively by the audio thread at run
 * time, except the config fields (software_relay, tx_delay_ms,
 * tx_tail_ms, relay_drain_ms) which are written by the main thread
 * during configure() / config-reload.
 *
 * A few fields are ALSO read by the web status thread via
 * get_tx_state() in src/main.c. Those reads are unsynchronised and
 * harmlessly racy today (32-bit scalar reads; worst case the status
 * label is a tick stale). Preserving that property is a non-goal of
 * the refactor — if it becomes a problem we add atomics later.
 */
typedef struct kerchunk_audio_state_s {
    /* ── Config (written at configure/reload, read on audio thread) ── */
    int software_relay;      /* 1 = retransmit RX through TX path */
    int tx_delay_ms;         /* silence after PTT assert, before audio */
    int tx_tail_ms;          /* silence after last audio, before PTT release */
    int relay_drain_ms;      /* drain window after COR drops in relay mode */

    /* ── Software-relay state (audio thread) ── */
    int relay_drain;         /* remaining drain samples (>0 while draining) */
    int relay_was_active;    /* prev-tick relay_active for edge detect */

    /* ── Queue TX state (audio thread, read by status API) ── */
    int queue_ptt;           /* 1 while the queue holds a PTT ref */
    int queue_fired_drain;   /* 1 after QUEUE_DRAIN event fired */
    uint64_t queue_drain_start_us;

    /* ── TX cadence (audio thread, read by status API) ── */
    int tx_delay_rem;        /* remaining tx_delay samples */
    int tx_tail_rem;         /* remaining tx_tail samples (-1 = idle) */
    int ptt_hold_ticks;      /* ticks held after playback ring empties */
} kerchunk_audio_state_t;


/*
 * ── RX sub-tick (PLAN-AUDIO-TICK.md Phase 2) ─────────────────────
 *
 * Pure function over the audio state + a small rx-sub-state + one
 * RX frame. No I/O, no event-bus calls, no global reads. Tests
 * drive it with scripted frames and assert on the returned out
 * struct; the shell (audio_thread_fn in src/main.c) translates
 * the outputs into the actual calls (kerchevt_fire,
 * atomic_store(&g_tx_dtmf_active, ...), kerchunk_audio_playback,
 * and the playback-tap dispatch).
 *
 * Borrowed pointer: plcode_dtmf_dec_t *. The function may call
 * plcode_dtmf_dec_reset() and plcode_dtmf_dec_process() on it.
 * The caller owns the decoder and its lifecycle.
 *
 * Note: frame is INPUT to the DTMF decoder only. The relay copy +
 * TX scrambler + playback write live in the shell so scrambler
 * hook lookups stay out of the pure function.
 */

typedef struct kerchunk_audio_tick_rx_state_s {
    int prev_dtmf;           /* decoder detected-flag carried across ticks */
} kerchunk_audio_tick_rx_state_t;

typedef enum {
    KERCHUNK_TICK_RX_NO_EVENT = 0,
    KERCHUNK_TICK_RX_DTMF_DIGIT,
    KERCHUNK_TICK_RX_DTMF_END,
} kerchunk_tick_rx_event_t;

typedef struct kerchunk_audio_tick_rx_out_s {
    /* Value to store to g_tx_dtmf_active (atomic publish to main thread) */
    int dtmf_active;

    /* DTMF event to fire this tick (NO_EVENT, DIGIT, or END) */
    kerchunk_tick_rx_event_t event;
    char                      digit;   /* valid iff event == DTMF_DIGIT */

    /* Relay output intent. When non-zero, the shell should:
     *   1. copy `frame` into a scratch buffer
     *   2. apply the TX scrambler to the copy
     *   3. write the copy to kerchunk_audio_playback()
     *   4. dispatch the copy to playback taps */
    int relay_write;
} kerchunk_audio_tick_rx_out_t;

/*
 * Run the RX sub-tick. Byte-for-byte equivalent to the RX path that
 * used to live inline in audio_thread_fn in src/main.c (lines
 * ~1044-1198 pre-Phase 2). Call once per captured frame, including
 * under the RX catch-up burst.
 *
 * Parameters
 *   s              — audio-thread state (relay_drain etc. mutated here)
 *   rx             — rx-sub-state (prev_dtmf mutated here)
 *   dec            — borrowed DTMF decoder; reset/process called here
 *   frame, nread   — captured RX frame (already descrambled by caller)
 *   relay_active   — core->is_receiving() for this tick
 *   ptt_held       — core_get_ptt() for this tick
 *   play_writable  — kerchunk_audio_playback_writable()
 *   sample_rate    — for relay_drain_ms→samples conversion on COR drop
 *   out            — filled with this tick's events + relay intent
 */
void kerchunk_audio_tick_rx(kerchunk_audio_state_t *s,
                            kerchunk_audio_tick_rx_state_t *rx,
                            plcode_dtmf_dec_t *dec,
                            const int16_t *frame,
                            int nread,
                            int relay_active,
                            int ptt_held,
                            size_t play_writable,
                            int sample_rate,
                            kerchunk_audio_tick_rx_out_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KERCHUNK_AUDIO_TICK_H */
