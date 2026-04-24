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

#ifdef __cplusplus
}
#endif

#endif /* KERCHUNK_AUDIO_TICK_H */
