/*
 * kerchunk_audio_ring.h — SPSC capture ring + PA-callback commit.
 *
 * Lifted out of src/kerchunk_audio.c in PLAN-AUDIO-TICK.md Phase 1
 * so the underflow-drop and capture-resample paths can be unit tested
 * without standing up PortAudio.
 *
 * The ring itself is a single-producer single-consumer lock-free
 * structure: the PortAudio callback writes, the audio thread reads.
 * Atomicity is per-field (head/tail), no locks on the data path.
 */

#ifndef KERCHUNK_AUDIO_RING_H
#define KERCHUNK_AUDIO_RING_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KERCHUNK_AUDIO_RING_SIZE 262144u   /* ~5.5 s at 48 kHz, must be power of 2 */
#define KERCHUNK_AUDIO_RING_MASK (KERCHUNK_AUDIO_RING_SIZE - 1u)

typedef struct kerchunk_audio_ring_s {
    int16_t       buf[KERCHUNK_AUDIO_RING_SIZE];
    atomic_size_t head;     /* producer write position */
    atomic_size_t tail;     /* consumer read position */
} kerchunk_audio_ring_t;

void   kerchunk_audio_ring_init(kerchunk_audio_ring_t *r);
size_t kerchunk_audio_ring_readable(const kerchunk_audio_ring_t *r);
size_t kerchunk_audio_ring_writable(const kerchunk_audio_ring_t *r);
size_t kerchunk_audio_ring_write(kerchunk_audio_ring_t *r,
                                 const int16_t *src, size_t n);
size_t kerchunk_audio_ring_read(kerchunk_audio_ring_t *r,
                                int16_t *dst, size_t n);

/*
 * Commit a PortAudio capture buffer to the ring.
 *
 * Behavior:
 *   - If `underflow` is non-zero, the buffer is dropped wholesale and
 *     the function returns 0. (PA flagged paInputUnderflow → the
 *     sound card couldn't deliver fresh samples; the buffer it gave
 *     us is zero-filled or stale, and the audio thread will
 *     repeat-last to cover the gap.)
 *   - If `hw_rate == target_rate`, samples are written directly.
 *   - Otherwise the buffer is linearly resampled hw_rate→target_rate
 *     using `*resample_pos` as a persistent fractional position
 *     across calls (so phase continuity is preserved across PA
 *     callback boundaries).
 *
 * Returns: total samples written into the ring (post-resample). May
 * be less than expected if the ring is full — silent partial-write
 * matches the original cap_cb behavior.
 *
 * Pure function over (ring, resample_pos). No globals, no I/O.
 * Safe to call from a real-time PA callback.
 */
size_t kerchunk_audio_ring_commit(kerchunk_audio_ring_t *ring,
                                  const int16_t *src,
                                  size_t n,
                                  int hw_rate,
                                  int target_rate,
                                  double *resample_pos,
                                  int underflow);

/*
 * Fill `dst` with samples repeated from `last_buf` — the primitive
 * behind kerchunk_audio_capture_repeat_last() in kerchunk_audio.c.
 *
 * Cases:
 *   - last_n == 0 → dst is zero-filled (no prior-good frame yet).
 *   - last_n >= dst_n → copy the TAIL of last_buf (dst_n samples).
 *   - last_n <  dst_n → tile last_buf[0..last_n] across dst until
 *                       dst_n is covered.
 *
 * Pure function; no state. Audio-thread reuses this to avoid
 * zero-padding capture under-runs, which would break the DTMF
 * decoder's 2-block hysteresis across a silence injection.
 */
void kerchunk_audio_repeat_fill(int16_t *dst, size_t dst_n,
                                const int16_t *last_buf, size_t last_n);

#ifdef __cplusplus
}
#endif

#endif /* KERCHUNK_AUDIO_RING_H */
