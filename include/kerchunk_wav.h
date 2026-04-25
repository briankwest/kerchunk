/*
 * kerchunk_wav.h — WAV file read/write
 */

#ifndef KERCHUNK_WAV_H
#define KERCHUNK_WAV_H

#include <stdint.h>
#include <stddef.h>

/* Read a WAV file into a buffer.
 * path:      File path.
 * buf:       Receives allocated sample buffer (caller must free).
 * n:         Receives number of samples.
 * rate:      Receives sample rate.
 * Returns 0 on success, -1 on error.
 * Converts to mono 16-bit if needed. */
int  kerchunk_wav_read(const char *path, int16_t **buf, size_t *n, int *rate);

/* Write samples to a WAV file.
 * path:      File path.
 * buf:       Sample buffer.
 * n:         Number of samples.
 * rate:      Sample rate.
 * Returns 0 on success, -1 on error. */
int  kerchunk_wav_write(const char *path, const int16_t *buf, size_t n, int rate);

/* Read raw PCM file (headerless, mono 16-bit signed LE).
 * Returns 0 on success. */
int  kerchunk_pcm_read(const char *path, int16_t **buf, size_t *n);

/* Write raw PCM file.
 * Returns 0 on success. */
int  kerchunk_pcm_write(const char *path, const int16_t *buf, size_t n);

/* One-shot resample: anti-alias LPF + linear interp. Allocates
 * output (caller frees). Filter state starts fresh on each call —
 * fine for full-buffer operations like WAV file loading. For
 * streaming use (per-frame audio paths), use the streaming API
 * below to preserve filter state across calls.
 * Returns 0 on success, -1 on error. */
int  kerchunk_resample(const int16_t *src, size_t src_n, int src_rate,
                       int dst_rate, int16_t **dst, size_t *dst_n);

/* Streaming resampler. Holds biquad anti-alias LPF state and
 * interp position across calls so back-to-back frames don't
 * introduce boundary clicks or filter restart artifacts.
 *
 *   kerchunk_resampler_t *r = kerchunk_resampler_create(48000, 8000);
 *   for each frame:
 *       int16_t out[160];
 *       size_t out_n = kerchunk_resampler_process(r, in, 960, out, 160);
 *   kerchunk_resampler_destroy(r);
 *
 * The LPF cutoff is set to 0.45 * min(src_rate, dst_rate) to
 * suppress alias/image components when going down or up. */
typedef struct kerchunk_resampler kerchunk_resampler_t;

kerchunk_resampler_t *kerchunk_resampler_create(int src_rate, int dst_rate);
void                  kerchunk_resampler_destroy(kerchunk_resampler_t *r);

/* Process one chunk. Returns number of output samples written. */
size_t                kerchunk_resampler_process(kerchunk_resampler_t *r,
                                                  const int16_t *src, size_t src_n,
                                                  int16_t *dst, size_t dst_max);

#endif /* KERCHUNK_WAV_H */
