/*
 * kerchunk_audio_ring.c — SPSC ring + PA-callback commit.
 *
 * See include/kerchunk_audio_ring.h for the contract. Lifted from
 * src/kerchunk_audio.c in PLAN-AUDIO-TICK.md Phase 1.
 */

#include "kerchunk_audio_ring.h"

#include <string.h>

void kerchunk_audio_ring_init(kerchunk_audio_ring_t *r)
{
    memset(r->buf, 0, sizeof(r->buf));
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
}

size_t kerchunk_audio_ring_readable(const kerchunk_audio_ring_t *r)
{
    return (atomic_load(&r->head) - atomic_load(&r->tail))
           & KERCHUNK_AUDIO_RING_MASK;
}

size_t kerchunk_audio_ring_writable(const kerchunk_audio_ring_t *r)
{
    return KERCHUNK_AUDIO_RING_SIZE - 1u - kerchunk_audio_ring_readable(r);
}

size_t kerchunk_audio_ring_write(kerchunk_audio_ring_t *r,
                                 const int16_t *src, size_t n)
{
    size_t space = kerchunk_audio_ring_writable(r);
    if (n > space) n = space;
    size_t h = atomic_load(&r->head);
    for (size_t i = 0; i < n; i++)
        r->buf[(h + i) & KERCHUNK_AUDIO_RING_MASK] = src[i];
    atomic_store(&r->head, (h + n) & KERCHUNK_AUDIO_RING_MASK);
    return n;
}

size_t kerchunk_audio_ring_read(kerchunk_audio_ring_t *r,
                                int16_t *dst, size_t n)
{
    size_t avail = kerchunk_audio_ring_readable(r);
    if (n > avail) n = avail;
    size_t t = atomic_load(&r->tail);
    for (size_t i = 0; i < n; i++)
        dst[i] = r->buf[(t + i) & KERCHUNK_AUDIO_RING_MASK];
    atomic_store(&r->tail, (t + n) & KERCHUNK_AUDIO_RING_MASK);
    return n;
}

size_t kerchunk_audio_ring_commit(kerchunk_audio_ring_t *ring,
                                  const int16_t *src,
                                  size_t n,
                                  int hw_rate,
                                  int target_rate,
                                  double *resample_pos,
                                  int underflow)
{
    /* PA flagged paInputUnderflow → buffer is zero-filled / stale.
     * Drop it wholesale; the audio thread's repeat-last path covers
     * the gap with continuous-looking audio so the DTMF decoder's
     * hysteresis doesn't see an injected silence dropout. */
    if (underflow) return 0;

    /* Defensive: NULL/zero-length is a no-op rather than UB. */
    if (!ring || !src || n == 0) return 0;

    if (hw_rate == target_rate) {
        return kerchunk_audio_ring_write(ring, src, n);
    }

    /* Downsample (or upsample) hw_rate → target_rate with persistent
     * fractional position. Phase continuity is preserved across PA
     * callback boundaries via *resample_pos. */
    if (!resample_pos) return 0;  /* resampler needs persistent state */

    const double step = (double)hw_rate / (double)target_rate;
    int16_t tmp[512];
    size_t  out_i = 0;
    size_t  written = 0;

    while (*resample_pos < (double)n) {
        size_t idx  = (size_t)(*resample_pos);
        double frac = *resample_pos - (double)idx;
        int16_t s0 = (idx     < n) ? src[idx]     : 0;
        int16_t s1 = (idx + 1 < n) ? src[idx + 1] : s0;
        tmp[out_i++] = (int16_t)(s0 + frac * (s1 - s0));

        if (out_i == 512) {
            written += kerchunk_audio_ring_write(ring, tmp, 512);
            out_i = 0;
        }
        *resample_pos += step;
    }
    *resample_pos -= (double)n;  /* carry fractional remainder */

    if (out_i > 0)
        written += kerchunk_audio_ring_write(ring, tmp, out_i);

    return written;
}
