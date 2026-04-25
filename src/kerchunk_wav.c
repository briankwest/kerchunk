/*
 * kerchunk_wav.c — WAV file read/write (16-bit PCM)
 */

#include "kerchunk_wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Linear-interp resample into a caller-provided buffer. Same
 * algorithm used by both kerchunk_resample (which malloc's the
 * destination) and direct callers in the audio hot path.
 * Bounds output by dst_max so a too-small destination doesn't
 * overrun. */
size_t kerchunk_resample_into(int16_t *dst, size_t dst_max,
                               const int16_t *src, size_t src_n,
                               int src_rate, int dst_rate)
{
    if (!dst || !src || src_rate <= 0 || dst_rate <= 0 || dst_max == 0)
        return 0;

    if (src_rate == dst_rate) {
        size_t n = src_n < dst_max ? src_n : dst_max;
        memcpy(dst, src, n * sizeof(int16_t));
        return n;
    }

    size_t out_n = (size_t)((double)src_n * dst_rate / src_rate);
    if (out_n == 0) out_n = 1;
    if (out_n > dst_max) out_n = dst_max;

    double step = (double)src_rate / (double)dst_rate;
    double pos = 0.0;
    for (size_t i = 0; i < out_n; i++) {
        size_t idx = (size_t)pos;
        double frac = pos - (double)idx;
        int16_t s0 = (idx < src_n) ? src[idx] : 0;
        int16_t s1 = (idx + 1 < src_n) ? src[idx + 1] : s0;
        dst[i] = (int16_t)(s0 + frac * (s1 - s0));
        pos += step;
    }
    return out_n;
}

int kerchunk_resample(const int16_t *src, size_t src_n, int src_rate,
                      int dst_rate, int16_t **dst, size_t *dst_n)
{
    if (!src || !dst || !dst_n || src_rate <= 0 || dst_rate <= 0)
        return -1;

    /* Allocate worst-case output, then call the no-alloc variant. */
    size_t cap = (src_rate == dst_rate)
                 ? src_n
                 : (size_t)((double)src_n * dst_rate / src_rate);
    if (cap == 0) cap = 1;
    int16_t *out = malloc(cap * sizeof(int16_t));
    if (!out) return -1;

    size_t got = kerchunk_resample_into(out, cap, src, src_n, src_rate, dst_rate);
    *dst = out;
    *dst_n = got;
    return 0;
}
