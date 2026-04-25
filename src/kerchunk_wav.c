/*
 * kerchunk_wav.c — WAV file read/write (16-bit PCM)
 */

#include "kerchunk_wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* WAV header (44 bytes for PCM) */
typedef struct __attribute__((packed)) {
    char     riff[4];        /* "RIFF" */
    uint32_t file_size;      /* file size - 8 */
    char     wave[4];        /* "WAVE" */
    char     fmt_id[4];      /* "fmt " */
    uint32_t fmt_size;       /* 16 for PCM */
    uint16_t audio_format;   /* 1 = PCM */
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_id[4];     /* "data" */
    uint32_t data_size;
} wav_header_t;

int kerchunk_wav_read(const char *path, int16_t **buf, size_t *n, int *rate)
{
    if (!path || !buf || !n || !rate)
        return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;

    wav_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        fclose(fp);
        return -1;
    }

    /* Validate RIFF/WAVE header */
    if (memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0) {
        fclose(fp);
        return -1;
    }
    if (hdr.audio_format != 1 || hdr.bits_per_sample != 16) {
        fclose(fp);
        return -1;
    }
    if (hdr.channels == 0 || hdr.channels > 2) {
        fclose(fp);
        return -1;
    }
    /* Cap data_size to prevent excessive allocation (100 MB) */
    if (hdr.data_size > 100 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }
    if (hdr.fmt_size > 1024) {
        fclose(fp);
        return -1;
    }

    /* Handle extended fmt chunks by seeking to data */
    if (memcmp(hdr.data_id, "data", 4) != 0) {
        /* Seek past fmt chunk and find data chunk */
        fseek(fp, 12 + 8 + hdr.fmt_size, SEEK_SET);
        char chunk_id[4];
        uint32_t chunk_size;
        while (fread(chunk_id, 1, 4, fp) == 4 && fread(&chunk_size, 1, 4, fp) == 4) {
            if (memcmp(chunk_id, "data", 4) == 0) {
                hdr.data_size = chunk_size;
                break;
            }
            fseek(fp, chunk_size, SEEK_CUR);
        }
    }

    size_t samples = hdr.data_size / (hdr.channels * sizeof(int16_t));
    int16_t *samples_buf = malloc(samples * sizeof(int16_t));
    if (!samples_buf) {
        fclose(fp);
        return -1;
    }

    if (hdr.channels == 1) {
        if (fread(samples_buf, sizeof(int16_t), samples, fp) != samples) {
            free(samples_buf);
            fclose(fp);
            return -1;
        }
    } else {
        /* Mix to mono */
        int16_t *interleaved = malloc(hdr.data_size);
        if (!interleaved) {
            free(samples_buf);
            fclose(fp);
            return -1;
        }
        size_t total_frames = samples;
        size_t total_samples = total_frames * hdr.channels;
        if (fread(interleaved, sizeof(int16_t), total_samples, fp) != total_samples) {
            free(interleaved);
            free(samples_buf);
            fclose(fp);
            return -1;
        }
        for (size_t i = 0; i < total_frames; i++) {
            int32_t sum = 0;
            for (int ch = 0; ch < hdr.channels; ch++)
                sum += interleaved[i * hdr.channels + ch];
            samples_buf[i] = (int16_t)(sum / hdr.channels);
        }
        free(interleaved);
    }

    *buf  = samples_buf;
    *n    = samples;
    *rate = hdr.sample_rate;
    fclose(fp);
    return 0;
}

int kerchunk_wav_write(const char *path, const int16_t *buf, size_t n, int rate)
{
    if (!path || !buf || n == 0 || rate <= 0)
        return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;

    uint32_t data_size = (uint32_t)(n * sizeof(int16_t));
    wav_header_t hdr;
    memcpy(hdr.riff, "RIFF", 4);
    hdr.file_size = data_size + sizeof(hdr) - 8;
    memcpy(hdr.wave, "WAVE", 4);
    memcpy(hdr.fmt_id, "fmt ", 4);
    hdr.fmt_size       = 16;
    hdr.audio_format   = 1;
    hdr.channels       = 1;
    hdr.sample_rate    = rate;
    hdr.byte_rate      = rate * sizeof(int16_t);
    hdr.block_align    = sizeof(int16_t);
    hdr.bits_per_sample = 16;
    memcpy(hdr.data_id, "data", 4);
    hdr.data_size = data_size;

    if (fwrite(&hdr, 1, sizeof(hdr), fp) != sizeof(hdr) ||
        fwrite(buf, sizeof(int16_t), n, fp) != n) {
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0) return -1;
    return 0;
}

int kerchunk_pcm_read(const char *path, int16_t **buf, size_t *n)
{
    if (!path || !buf || !n)
        return -1;

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0 || size % sizeof(int16_t) != 0) {
        fclose(fp);
        return -1;
    }

    size_t samples = size / sizeof(int16_t);
    int16_t *samples_buf = malloc(size);
    if (!samples_buf) {
        fclose(fp);
        return -1;
    }

    if (fread(samples_buf, sizeof(int16_t), samples, fp) != samples) {
        free(samples_buf);
        fclose(fp);
        return -1;
    }

    *buf = samples_buf;
    *n   = samples;
    fclose(fp);
    return 0;
}

int kerchunk_pcm_write(const char *path, const int16_t *buf, size_t n)
{
    if (!path || !buf || n == 0)
        return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;

    if (fwrite(buf, sizeof(int16_t), n, fp) != n) {
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/* ====================================================================
 * Resampler with anti-alias / anti-image filtering.
 *
 * Going from any rate to any other rate requires a low-pass filter
 * at min(src,dst)/2 to suppress (a) alias artifacts when downsampling
 * and (b) image artifacts when upsampling. Naive linear interpolation
 * with no filter folds the upper half-band back into audible range —
 * heard as "muddy" / "tinny" / consonants turning to mush.
 *
 * Implementation: 2nd-order Butterworth biquad LPF (12 dB/oct) at
 * 0.45 * min(src,dst) cutoff, applied at the higher of the two rates
 * (input side for downsample, after interpolation for upsample), then
 * linear interp between filtered samples to reach the target rate.
 *
 * The streaming API holds the biquad state + a saved input sample
 * across calls so back-to-back frames don't introduce restart
 * artifacts at frame boundaries.
 * ==================================================================== */

struct kerchunk_resampler {
    int    src_rate;
    int    dst_rate;
    /* Biquad direct-form-II-transposed state. Coefficients normalized
     * by a0; b0/b1/b2 numerator, a1/a2 denominator (without leading 1). */
    double b0, b1, b2, a1, a2;
    double z1, z2;
    /* Streaming interp: position within the (filtered) input stream
     * relative to the current call's starting offset. */
    double pos;
    /* Last filtered input sample, used as the "left side" for the
     * first interp output of the next call. */
    double prev_y;
    int    have_prev;
};

/* Compute Butterworth biquad coefficients for a low-pass at fc/fs. */
static void biquad_design(struct kerchunk_resampler *r, double fc, double fs)
{
    double w0 = 2.0 * M_PI * fc / fs;
    double cw = cos(w0);
    double sw = sin(w0);
    double Q  = 0.7071067811865475; /* 1/sqrt(2) — Butterworth */
    double alpha = sw / (2.0 * Q);
    double a0 = 1.0 + alpha;
    r->b0 = ((1.0 - cw) * 0.5) / a0;
    r->b1 = (1.0 - cw) / a0;
    r->b2 = ((1.0 - cw) * 0.5) / a0;
    r->a1 = (-2.0 * cw) / a0;
    r->a2 = (1.0 - alpha) / a0;
}

/* Direct-form-II transposed biquad step. */
static inline double biquad_step(struct kerchunk_resampler *r, double x)
{
    double y = r->b0 * x + r->z1;
    r->z1    = r->b1 * x - r->a1 * y + r->z2;
    r->z2    = r->b2 * x - r->a2 * y;
    return y;
}

/* Saturation clamp to int16 range. */
static inline int16_t sat16(double v)
{
    if (v >  32767.0) return  32767;
    if (v < -32768.0) return -32768;
    return (int16_t)v;
}

kerchunk_resampler_t *kerchunk_resampler_create(int src_rate, int dst_rate)
{
    if (src_rate <= 0 || dst_rate <= 0) return NULL;
    struct kerchunk_resampler *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->src_rate = src_rate;
    r->dst_rate = dst_rate;

    if (src_rate != dst_rate) {
        /* Cutoff: 0.45 of the lower rate (90% of that rate's Nyquist).
         * Apply at the higher rate so the filter operates on the
         * stream that contains the alias/image components we want
         * to suppress. */
        double low_rate  = (src_rate < dst_rate) ? src_rate : dst_rate;
        double high_rate = (src_rate > dst_rate) ? src_rate : dst_rate;
        biquad_design(r, low_rate * 0.45, high_rate);
    }
    return r;
}

void kerchunk_resampler_destroy(kerchunk_resampler_t *r)
{
    free(r);
}

size_t kerchunk_resampler_process(kerchunk_resampler_t *r,
                                   const int16_t *src, size_t src_n,
                                   int16_t *dst, size_t dst_max)
{
    if (!r || !src || !dst || dst_max == 0) return 0;

    /* Same rate → straight copy through (no filter). */
    if (r->src_rate == r->dst_rate) {
        size_t n = src_n < dst_max ? src_n : dst_max;
        memcpy(dst, src, n * sizeof(int16_t));
        return n;
    }

    int down = (r->src_rate > r->dst_rate);

    if (down) {
        /* Downsample: filter every input sample, then linearly
         * interpolate filtered samples at the output rate. The
         * filter runs at src_rate (the "high" side) so the alias
         * energy is killed before decimation. */
        double step = (double)r->src_rate / (double)r->dst_rate;
        size_t out_i = 0;
        for (size_t i = 0; i < src_n && out_i < dst_max; i++) {
            double y = biquad_step(r, (double)src[i]);
            /* While the next output position falls within the
             * interval [i, i+1), emit interpolated samples. */
            while (r->pos < (double)i + 1.0 && out_i < dst_max) {
                if (!r->have_prev) {
                    /* First sample ever — no left neighbor; emit y. */
                    dst[out_i++] = sat16(y);
                } else {
                    double frac = r->pos - (double)i;
                    /* frac < 0 means the position is BEFORE i (in
                     * the previous-call tail); use prev_y as left. */
                    if (frac < 0.0) {
                        double t = 1.0 + frac;  /* 0..1 across [prev,y] */
                        dst[out_i++] = sat16(r->prev_y + t * (y - r->prev_y));
                    } else {
                        /* frac in [0,1) — left=y at offset i,
                         * right will be the next iteration's y.
                         * We can only emit at frac=0 here; otherwise
                         * we need the next sample. Defer. */
                        if (frac == 0.0)
                            dst[out_i++] = sat16(y);
                        else
                            break;
                    }
                }
                r->pos += step;
            }
            r->prev_y = y;
            r->have_prev = 1;
        }
        /* Re-base pos against the next call's input. */
        r->pos -= (double)src_n;
        return out_i;
    } else {
        /* Upsample: linearly interpolate src to the output rate,
         * then filter the result at dst_rate to suppress images
         * introduced by the interpolation. */
        double step = (double)r->src_rate / (double)r->dst_rate;
        size_t out_i = 0;
        while (out_i < dst_max) {
            if (r->pos >= (double)src_n) break;
            size_t idx = (size_t)r->pos;
            double frac = r->pos - (double)idx;
            double s0 = (idx == 0 && r->have_prev)
                        ? r->prev_y
                        : (double)src[idx == 0 ? 0 : (idx - 1)];
            /* Actually: idx is "left", idx+1 is "right" relative
             * to fractional pos within input. Adjust. */
            s0 = (double)src[idx];
            double s1 = (idx + 1 < src_n) ? (double)src[idx + 1]
                                          : (double)src[idx];
            double interp = s0 + frac * (s1 - s0);
            double y = biquad_step(r, interp);
            dst[out_i++] = sat16(y);
            r->pos += step;
        }
        if (src_n > 0) {
            r->prev_y = (double)src[src_n - 1];
            r->have_prev = 1;
            r->pos -= (double)src_n;
        }
        return out_i;
    }
}

/* One-shot wrapper — fresh resampler per call. Existing callers
 * (WAV file load) get correct output without code changes. */
int kerchunk_resample(const int16_t *src, size_t src_n, int src_rate,
                      int dst_rate, int16_t **dst, size_t *dst_n)
{
    if (!src || !dst || !dst_n || src_rate <= 0 || dst_rate <= 0)
        return -1;

    if (src_rate == dst_rate) {
        int16_t *out = malloc(src_n * sizeof(int16_t));
        if (!out) return -1;
        memcpy(out, src, src_n * sizeof(int16_t));
        *dst = out;
        *dst_n = src_n;
        return 0;
    }

    /* Allocate worst-case output. */
    size_t out_cap = (size_t)((double)src_n * dst_rate / src_rate) + 4;
    int16_t *out = malloc(out_cap * sizeof(int16_t));
    if (!out) return -1;

    kerchunk_resampler_t *r = kerchunk_resampler_create(src_rate, dst_rate);
    if (!r) { free(out); return -1; }

    size_t got = kerchunk_resampler_process(r, src, src_n, out, out_cap);
    kerchunk_resampler_destroy(r);

    *dst = out;
    *dst_n = got;
    return 0;
}
