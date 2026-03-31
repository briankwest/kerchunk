/*
 * kerchunk_audio.h — Audio engine (PortAudio)
 *
 * Cross-platform via PortAudio: macOS (CoreAudio), Linux (ALSA),
 * Windows (WASAPI/DS). Callback-driven with lock-free ring buffers.
 */

#ifndef KERCHUNK_AUDIO_H
#define KERCHUNK_AUDIO_H

#include <stdint.h>
#include <stddef.h>

/* Audio engine configuration */
typedef struct {
    const char *capture_device;    /* Device name/index or "default" */
    const char *playback_device;
    int sample_rate;               /* Target rate (8000) */
    int hw_rate;                   /* Force hardware rate (0=auto, 48000=recommended for USB) */
    int preemphasis;
    float preemphasis_alpha;
    int speaker_volume;            /* Speaker playback volume 0-151 (-1 = don't set) */
    int mic_volume;                /* Mic capture volume 0-16 (-1 = don't set) */
    int agc;                       /* Auto Gain Control: 0=off, 1=on, -1=don't set */
} kerchunk_audio_config_t;

/* Initialize audio engine. Returns 0 on success. */
int  kerchunk_audio_init(const kerchunk_audio_config_t *cfg);

/* Shutdown audio engine. */
void kerchunk_audio_shutdown(void);

/* Read samples from capture ring buffer (non-blocking).
 * Zero-fills if not enough data available yet.
 * Returns number of samples (always n). */
int  kerchunk_audio_capture(int16_t *buf, size_t n);

/* Write samples into playback ring buffer (non-blocking).
 * Drops samples if ring is full.
 * Returns number of samples (always n). */
int  kerchunk_audio_playback(const int16_t *buf, size_t n);

/* Apply pre-emphasis filter in-place. */
void kerchunk_audio_preemphasis(int16_t *buf, size_t n, float alpha);

/* Number of samples that can be written without dropping. */
size_t kerchunk_audio_playback_writable(void);

/* Number of samples in playback ring not yet consumed by hardware. */
size_t kerchunk_audio_playback_pending(void);

/* 1 if audio hardware is open and streaming. */
int  kerchunk_audio_available(void);

/* Print available audio devices to stderr. */
void kerchunk_audio_list_devices(void);

/* Return the target sample rate the audio engine is running at. */
int kerchunk_audio_get_rate(void);

#endif /* KERCHUNK_AUDIO_H */
