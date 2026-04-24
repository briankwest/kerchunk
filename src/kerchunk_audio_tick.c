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
