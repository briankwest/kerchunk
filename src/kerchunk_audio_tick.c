/*
 * kerchunk_audio_tick.c — RX sub-tick implementation.
 *
 * PLAN-AUDIO-TICK.md Phase 2. Behavior is byte-for-byte equivalent
 * to the RX path that used to live inline in audio_thread_fn in
 * src/main.c. See include/kerchunk_audio_tick.h for the contract.
 *
 * Ordering note: the original loop did (a) decoder-reset edge,
 * (b) decoder process + events, (c) COR-drop drain-start edge,
 * (d) update relay_was_active, (e) compute relay decision +
 * early-stop. This function preserves that exact order — a
 * reassertion of it would be a behavioral change.
 */

#include "kerchunk_audio_tick.h"

#include <string.h>

void kerchunk_audio_tick_rx(kerchunk_audio_state_t *s,
                            kerchunk_audio_tick_rx_state_t *rx,
                            plcode_dtmf_dec_t *dec,
                            const int16_t *frame,
                            int nread,
                            int relay_active,
                            int ptt_held,
                            size_t play_writable,
                            int sample_rate,
                            kerchunk_audio_tick_rx_out_t *out)
{
    if (!out) return;

    /* Conservative defaults — if we return early below, the shell
     * treats the tick as "nothing to do". */
    out->dtmf_active = 0;
    out->event       = KERCHUNK_TICK_RX_NO_EVENT;
    out->digit       = '\0';
    out->relay_write = 0;

    if (!s || !rx) return;

    /* ── 1. Decoder reset on COR assert BEFORE processing this tick's
     *       audio. Otherwise the first frame of a new transmission is
     *       fed into a decoder still holding Goertzel accumulators,
     *       hysteresis counters, and current_digit from the PREVIOUS
     *       transmission — letting a spurious DTMF_DIGIT leak into the
     *       new session (and intermittently locking the decoder onto
     *       the wrong digit for the first real tone). */
    if (relay_active && !s->relay_was_active && dec)
        plcode_dtmf_dec_reset(dec);

    /* ── 2. DTMF decoder: only process when COR active or draining.
     *       Saves CPU and prevents false detections from noise/silence.
     *       The relay drain window catches late digits in the squelch
     *       tail. */
    if ((relay_active || s->relay_drain > 0) && dec && frame && nread > 0) {
        plcode_dtmf_result_t res;
        plcode_dtmf_dec_process(dec, frame, (size_t)nread, &res);

        out->dtmf_active = res.detected ? 1 : 0;

        if (res.detected && !rx->prev_dtmf) {
            out->event = KERCHUNK_TICK_RX_DTMF_DIGIT;
            out->digit = res.digit;
        } else if (!res.detected && rx->prev_dtmf) {
            out->event = KERCHUNK_TICK_RX_DTMF_END;
        }
        rx->prev_dtmf = res.detected ? 1 : 0;
    } else {
        /* Decoder not running this tick — ensure the fused detector in
         * the main thread doesn't see stale detection state. Emit a
         * trailing END if a tone was still held when the signal
         * dropped. */
        out->dtmf_active = 0;
        if (rx->prev_dtmf) {
            out->event = KERCHUNK_TICK_RX_DTMF_END;
            rx->prev_dtmf = 0;
        }
    }

    /* ── 3. Detect COR drop → start drain countdown.
     *       Only applies in software-relay mode; on hardware relay
     *       the radio handles its own tail. */
    if (s->software_relay && s->relay_was_active && !relay_active) {
        s->relay_drain = (sample_rate * s->relay_drain_ms) / 1000;
    }

    /* ── 4. Update relay_was_active for next tick's edge detect.
     *       Must happen AFTER steps 1 and 3 read it. */
    s->relay_was_active = relay_active;

    /* ── 5. Relay decision: retransmit RX audio with TX encoder.
     *       Don't relay while queue is transmitting — any COR during
     *       queue TX is feedback from our own transmission, not a real
     *       signal. Without this guard the relay fills the playback
     *       ring with noise and the queue can never release PTT
     *       (playback_pending never reaches 0). */
    const int do_relay = s->software_relay && ptt_held &&
                         !s->queue_ptt &&
                         (relay_active || s->relay_drain > 0);

    if (do_relay && nread > 0 && frame &&
        play_writable >= (size_t)nread) {
        out->relay_write = 1;

        /* Count down drain timer after COR dropped. Early stop: if
         * captured audio is below noise floor, speech has ended —
         * skip remaining drain so courtesy tone plays immediately
         * instead of waiting the full relay_drain period. */
        if (!relay_active && s->relay_drain > 0) {
            s->relay_drain -= nread;
            int64_t pwr = 0;
            for (int k = 0; k < nread; k++)
                pwr += (int64_t)frame[k] * frame[k];
            if (pwr / nread < 200 * 200)   /* ~200 RMS ≈ noise floor */
                s->relay_drain = 0;
        }
    }
}

/* ── TX sub-tick ─────────────────────────────────────────────────
 *
 * Helpers to keep the main function readable. */

static void push_action(kerchunk_audio_tick_tx_out_t *out,
                        kerchunk_tx_action_kind_t kind,
                        int samples, uint32_t dur_ms)
{
    if (out->count >= KERCHUNK_TX_ACTIONS_MAX) return;  /* clamp */
    out->actions[out->count].kind        = kind;
    out->actions[out->count].samples     = samples;
    out->actions[out->count].duration_ms = dur_ms;
    out->count++;
}

void kerchunk_audio_tick_tx(kerchunk_audio_state_t *s,
                            const kerchunk_audio_tick_tx_in_t *in,
                            int sample_rate,
                            int frame_samples,
                            kerchunk_audio_tick_tx_out_t *out)
{
    if (!out) return;
    out->count = 0;
    out->rerun_this_tick = 0;
    if (!s || !in || sample_rate <= 0 || frame_samples <= 0) return;

    /* Queue pause: wait for COR clear AND any software-relay drain.
     * Applies REGARDLESS of software_relay mode — hardware-relay
     * setups also must not start queue TX while the user is still
     * keyed up. This is the item-#3 fix from ARCH-COR-DTMF.md §12. */
    const int queue_paused = !s->queue_ptt &&
                             (in->relay_active || s->relay_drain > 0);

    /* ── 1. Start PTT + tx_delay setup. ── */
    if (!queue_paused && !s->queue_ptt &&
        (in->queue_depth > 0 || in->queue_is_draining)) {
        /* If some OTHER subsystem already holds PTT (repeater tail,
         * web_ptt, etc.), we skip the tx_delay silence — the radio
         * is already transmitting. */
        const int other_holds = in->ptt_held;
        push_action(out, KERCHUNK_TX_ACT_ASSERT_PTT, 0, 0);
        s->queue_ptt          = 1;
        s->queue_fired_drain  = 0;
        s->tx_delay_rem       = other_holds
                                  ? 0
                                  : (sample_rate * s->tx_delay_ms) / 1000;
        s->tx_tail_rem        = -1;
    }

    /* Track remaining writable space across actions this tick. */
    size_t play_w = in->play_writable;

    /* ── 2. TX delay silence. Feed silence while the radio keys up. ── */
    while (!queue_paused && s->queue_ptt && s->tx_delay_rem > 0 &&
           play_w >= (size_t)frame_samples &&
           out->count < KERCHUNK_TX_ACTIONS_MAX) {
        int sn = s->tx_delay_rem < frame_samples
                 ? s->tx_delay_rem : frame_samples;
        push_action(out, KERCHUNK_TX_ACT_SILENCE, sn, 0);
        s->tx_delay_rem -= sn;
        play_w          -= sn;
    }

    /* ── 3. Queue drain — exactly 1 frame per tick (real-time rate).
     *       Both PortAudio and WebSocket get the same frame. No burst
     *       draining — keeps the browser ring stable and prevents the
     *       write pointer from lapping the read pointer.
     *
     *       Only emit DRAIN when the queue actually has something to
     *       pull (queue_depth > 0 OR queue_is_draining mid-item).
     *       Otherwise the tail branch needs to run.
     *
     *       The !(is_receiving() && !ptt_held) guard from the original
     *       is a safety belt for "we think queue_ptt=1 but PTT isn't
     *       really held". s->queue_ptt==1 means our shell has asserted;
     *       combined with in->ptt_held (raw snapshot) we check for the
     *       corruption case. */
    int drained_this_tick = 0;
    const int we_or_other_hold_ptt = s->queue_ptt || in->ptt_held;
    if (!queue_paused && s->queue_ptt && s->tx_delay_rem <= 0 &&
        !(in->relay_active && !we_or_other_hold_ptt) &&
        (in->queue_depth > 0 || in->queue_is_draining) &&
        out->count < KERCHUNK_TX_ACTIONS_MAX) {
        /* Fire QUEUE_DRAIN on first audio frame. Event ordering:
         * FIRE_DRAIN BEFORE the DRAIN action so subscribers see the
         * event as the audio frame arrives. */
        if (!s->queue_fired_drain) {
            s->queue_fired_drain    = 1;
            s->queue_drain_start_us = in->now_us;
            push_action(out, KERCHUNK_TX_ACT_FIRE_DRAIN, 0, 0);
        }
        push_action(out, KERCHUNK_TX_ACT_DRAIN, frame_samples, 0);
        drained_this_tick = 1;
        /* Conservatively assume one frame just got written to the ring. */
        if (play_w >= (size_t)frame_samples)
            play_w -= (size_t)frame_samples;
    }

    /* ── 4. TX tail + PTT release (queue has fully emptied). ──
     *
     * Two-pass protocol when the queue empties:
     *   Pass A: start tail, emit FIRE_COMPLETE, set rerun_this_tick.
     *           We return WITHOUT emitting tail silence / RELEASE,
     *           because event subscribers may enqueue new items and
     *           we need the shell to re-sample queue_depth first.
     *   Pass B: shell re-calls with fresh queue_depth. Either:
     *           - queue_depth > 0 → tail cancel; reset tail state
     *             and set rerun again (so a 3rd pass can drain).
     *           - queue_depth == 0 → proceed to tail silence + release. */
    if (!queue_paused && !drained_this_tick && s->queue_ptt &&
        s->tx_delay_rem <= 0 &&
        in->queue_depth == 0 && !in->queue_is_draining) {

        /* Pass A — first tick at end-of-queue, fires QUEUE_COMPLETE. */
        if (s->tx_tail_rem < 0) {
            s->tx_tail_rem = (sample_rate * s->tx_tail_ms) / 1000;
            if (s->queue_fired_drain) {
                uint32_t dur_ms = (uint32_t)((in->now_us -
                                              s->queue_drain_start_us) / 1000);
                push_action(out, KERCHUNK_TX_ACT_FIRE_COMPLETE, 0, dur_ms);
                s->queue_fired_drain = 0;
                out->rerun_this_tick = 1;
                return;
            }
            /* No FIRE_COMPLETE (queue never really drained audio —
             * e.g. all items loaded pre-completion). Fall through to
             * tail silence + release. */
        }

        /* Feed tail silence */
        while (s->tx_tail_rem > 0 &&
               play_w >= (size_t)frame_samples &&
               out->count < KERCHUNK_TX_ACTIONS_MAX) {
            int sn = s->tx_tail_rem < frame_samples
                     ? s->tx_tail_rem : frame_samples;
            push_action(out, KERCHUNK_TX_ACT_SILENCE, sn, 0);
            s->tx_tail_rem -= sn;
            play_w         -= sn;
        }

        /* Release PTT only when:
         *   - tail has been fully written (tx_tail_rem <= 0)
         *   - playback ring has drained (play_pending == 0)
         *   - we've held for 3 more ticks so PA/ALSA flushes its
         *     internal buffer (the CTCSS tail on the wire). */
        if (s->tx_tail_rem <= 0 && in->play_pending == 0) {
            if (++s->ptt_hold_ticks >= 3 &&
                out->count < KERCHUNK_TX_ACTIONS_MAX) {
                s->queue_ptt      = 0;
                s->tx_tail_rem    = -1;
                s->ptt_hold_ticks = 0;
                push_action(out, KERCHUNK_TX_ACT_RELEASE_PTT, 0, 0);
            }
        } else {
            s->ptt_hold_ticks = 0;
        }
    }

    /* Pass B entry: tail already started (tx_tail_rem > 0) and now
     * queue has new items → tail cancel. Must run at the TOP of the
     * tick body, not under the queue_depth==0 branch above. */
    if (!queue_paused && s->queue_ptt && s->tx_tail_rem > 0 &&
        in->queue_depth > 0) {
        s->tx_tail_rem       = -1;
        s->queue_fired_drain = 1;  /* prevent re-fire of QUEUE_DRAIN on cancel */
        s->ptt_hold_ticks    = 0;
        out->rerun_this_tick = 1;
        out->count = 0;  /* discard anything accumulated on this pass */
    }
}
