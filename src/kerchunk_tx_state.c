/*
 * kerchunk_tx_state.c — TX state machine
 *
 * See include/kerchunk_tx_state.h for the contract and rationale.
 */

#include "kerchunk_tx_state.h"

#include <stddef.h>

static const char *const g_names[] = {
    [KERCHUNK_TX_IDLE]  = "TX_IDLE",
    [KERCHUNK_TX_RELAY] = "TX_RELAY",
    [KERCHUNK_TX_DELAY] = "TX_DELAY",
    [KERCHUNK_TX_DRAIN] = "TX_DRAIN",
    [KERCHUNK_TX_TAIL]  = "TX_TAIL",
    [KERCHUNK_TX_HOLD]  = "TX_HOLD",
};

const char *kerchunk_tx_state_name(kerchunk_tx_state_t s)
{
    if ((int)s < 0 || (size_t)s >= sizeof(g_names) / sizeof(*g_names))
        return "TX_UNKNOWN";
    return g_names[s] ? g_names[s] : "TX_UNKNOWN";
}

kerchunk_tx_state_t kerchunk_tx_state_compute(
    const kerchunk_tx_state_inputs_t *in)
{
    if (!in) return KERCHUNK_TX_IDLE;

    /* ── RELAY ──
     * Software-relay mode is live-retransmitting user audio. Takes
     * precedence over queue-driven states (the TX-sub-tick queue-
     * pause guard ensures the queue doesn't assert PTT while COR
     * is active, so a queue_ptt=1 AND cor_active=1 AND
     * software_relay=1 state is physically impossible — but if it
     * ever occurs, RELAY wins as the more informative label). */
    if (in->cor_active && in->software_relay)
        return KERCHUNK_TX_RELAY;

    /* ── Software-relay drain ──
     * After COR drops in relay mode, the audio thread continues
     * draining RX audio to the playback ring for relay_drain
     * samples so the last words don't get clipped. Technically
     * still transmitting but no queue is involved — map to TAIL. */
    if (in->relay_drain > 0)
        return KERCHUNK_TX_TAIL;

    /* ── Queue-driven TX phases ──
     * While the queue holds its PTT ref, we're in one of DELAY /
     * DRAIN / TAIL depending on which countdown is active. Check
     * in order:
     *   tx_delay_rem > 0   ⇒ DELAY  (silence before first audio)
     *   tx_tail_rem  >= 0  ⇒ TAIL   (silence after queue empty)
     *   otherwise          ⇒ DRAIN  (actively feeding queued audio) */
    if (in->queue_ptt) {
        if (in->tx_delay_rem > 0)  return KERCHUNK_TX_DELAY;
        if (in->tx_tail_rem  >= 0) return KERCHUNK_TX_TAIL;
        return KERCHUNK_TX_DRAIN;
    }

    /* ── HOLD ──
     * Queue released its PTT ref (queue_ptt=0) but the core
     * refcount is still above zero — that's the ptt_hold_ticks
     * window (60 ms) waiting for PortAudio + the CM119 USB
     * buffer to drain before the HID pin flips. */
    if (in->ptt_held)
        return KERCHUNK_TX_HOLD;

    return KERCHUNK_TX_IDLE;
}
