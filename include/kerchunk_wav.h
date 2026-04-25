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

/* Resample audio from one rate to another (linear interpolation).
 * Allocates output buffer — caller must free *dst.
 * Returns 0 on success, -1 on error. */
int  kerchunk_resample(const int16_t *src, size_t src_n, int src_rate,
                       int dst_rate, int16_t **dst, size_t *dst_n);

/* Same algorithm as kerchunk_resample but writes into a caller-
 * provided buffer (no allocation). For use in audio hot paths
 * that already have a stack array sized for the worst case.
 * Returns the number of output samples written (≤ dst_max). */
size_t kerchunk_resample_into(int16_t *dst, size_t dst_max,
                               const int16_t *src, size_t src_n,
                               int src_rate, int dst_rate);

#endif /* KERCHUNK_WAV_H */
