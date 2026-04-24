/*
 * kerchunk_audio.c — PortAudio callback engine with lock-free ring buffers
 *
 * PortAudio's audio thread calls our callbacks at hardware rate.
 * Capture callback  → writes into capture ring  → main/audio thread reads.
 * Main/audio thread → writes into playback ring → playback callback reads.
 *
 * Depends: libportaudio (portaudio-2.0 via pkg-config)
 *   macOS:  brew install portaudio
 *   Linux:  apt install libportaudio2-dev
 */

#include "kerchunk_audio.h"
#include "kerchunk_audio_ring.h"
#include "kerchunk.h"  /* for KERCHUNK_MAX_FRAME_SAMPLES */
#include "kerchunk_log.h"
#include <portaudio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __linux__
#include <alsa/asoundlib.h>
#endif

#define LOG_MOD "audio"

/* ── Pre-emphasis filter state ── */

static int     g_preemph_on;
static float   g_preemph_alpha;
static int16_t g_preemph_prev;

/* Last good capture frame — used to fill ring under-runs with
 * repeated samples instead of zeros so the DTMF decoder's hysteresis
 * survives a missing tick mid-tone (and so the relay path / web
 * stream / recordings don't get an audible click on under-runs). */
static int16_t g_cap_last[KERCHUNK_MAX_FRAME_SAMPLES];
static size_t  g_cap_last_n = 0;

void kerchunk_audio_preemphasis(int16_t *buf, size_t n, float alpha)
{
    for (size_t i = 0; i < n; i++) {
        int32_t out = buf[i] - (int32_t)(alpha * g_preemph_prev);
        if (out > 32767) out = 32767;
        if (out < -32768) out = -32768;
        g_preemph_prev = buf[i];
        buf[i] = (int16_t)out;
    }
}

/* ── PortAudio state ──
 *
 * SPSC ring impl lives in src/kerchunk_audio_ring.c (Phase 1 of
 * PLAN-AUDIO-TICK.md). The ring-commit function there handles the
 * underflow drop + capture resample paths that used to live inline
 * in cap_cb / duplex_cb below. */

static PaStream *g_cap_stream;
static PaStream *g_play_stream;
static kerchunk_audio_ring_t g_cap_ring;
static kerchunk_audio_ring_t g_play_ring;
static int       g_rate;        /* Target rate (8000) */
static int       g_hw_rate;     /* Actual hardware rate (may differ) */
static int       g_available;
static int       g_pa_init;

/* ── Resampling state (when hardware rate != target rate) ── */

static double  g_cap_resample_pos;   /* Fractional position in capture downsample */
static double  g_play_resample_pos; /* Fractional position in playback upsample */
static int16_t g_play_prev;         /* Previous sample for playback interpolation */
static int16_t g_play_cur;          /* Current sample for playback interpolation */

static PaStream *g_duplex_stream;    /* Non-NULL when using combined stream */

/* ── Callbacks (run on PortAudio's real-time audio thread) ── */

/* Combined full-duplex callback: capture + playback in one call.
 * Used when capture and playback are the same device — shared clock,
 * zero drift. */
static int duplex_cb(const void *in, void *out, unsigned long n,
                     const PaStreamCallbackTimeInfo *ti,
                     PaStreamCallbackFlags fl, void *ud)
{
    (void)ti; (void)ud;

    /* Capture side — skip if PA flagged input underflow (zero-filled
     * buffer); the audio thread's repeat-last path covers the gap.
     * Same rationale as cap_cb. */
    if (in) {
        kerchunk_audio_ring_commit(&g_cap_ring,
                                   (const int16_t *)in, n,
                                   g_hw_rate, g_rate,
                                   &g_cap_resample_pos,
                                   (fl & paInputUnderflow) ? 1 : 0);
    }

    /* Playback side — log output under-run once per second so a
     * stuttering CM119 doesn't flood the log but a real scheduling
     * problem is visible at the default log level. The recording
     * tap sees clean buffers even when this fires, so a clean TX
     * recording + a non-zero underflow counter here is the
     * signature of "kerchunk did its job; PA couldn't deliver". */
    if (fl & paOutputUnderflow) {
        static uint64_t last_log_us = 0;
        static unsigned underflow_count = 0;
        underflow_count++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_us = (uint64_t)now.tv_sec * 1000000u +
                          (uint64_t)now.tv_nsec / 1000u;
        if (now_us - last_log_us > 1000000u) {
            KERCHUNK_LOG_W(LOG_MOD,
                "PA output underflow (duplex): %u events in last second, "
                "n=%lu — audio thread missed the playback deadline",
                underflow_count, n);
            last_log_us = now_us;
            underflow_count = 0;
        }
    }

    /* Playback side */
    if (out) {
        int16_t *dst = (int16_t *)out;
        if (g_hw_rate == g_rate) {
            size_t got = kerchunk_audio_ring_read(&g_play_ring, dst, n);
            if (got < n) memset(dst + got, 0, (n - got) * sizeof(int16_t));
        } else {
            double step = (double)g_rate / (double)g_hw_rate;
            for (unsigned long i = 0; i < n; i++) {
                while (g_play_resample_pos >= 1.0) {
                    g_play_prev = g_play_cur;
                    int16_t s;
                    if (kerchunk_audio_ring_read(&g_play_ring, &s, 1) == 1) g_play_cur = s;
                    else g_play_cur = g_play_cur / 2;  /* Ramp down on underrun */
                    g_play_resample_pos -= 1.0;
                }
                double frac = g_play_resample_pos;
                dst[i] = (int16_t)((1.0 - frac) * g_play_prev + frac * g_play_cur);
                g_play_resample_pos += step;
            }
        }
    }

    return paContinue;
}

static int cap_cb(const void *in, void *out, unsigned long n,
                  const PaStreamCallbackTimeInfo *ti,
                  PaStreamCallbackFlags fl, void *ud)
{
    (void)out; (void)ti; (void)ud;
    if (!in) return paContinue;

    /* Commit this callback's buffer to the capture ring. Drops the
     * buffer on paInputUnderflow (zero-filled / stale input), resamples
     * hw_rate → target_rate when they differ. See
     * include/kerchunk_audio_ring.h for the contract. */
    kerchunk_audio_ring_commit(&g_cap_ring,
                               (const int16_t *)in, n,
                               g_hw_rate, g_rate,
                               &g_cap_resample_pos,
                               (fl & paInputUnderflow) ? 1 : 0);
    return paContinue;
}

static int play_cb(const void *in, void *out, unsigned long n,
                   const PaStreamCallbackTimeInfo *ti,
                   PaStreamCallbackFlags fl, void *ud)
{
    (void)in; (void)ti; (void)ud;
    int16_t *dst = (int16_t *)out;

    /* paOutputUnderflow: PA pulled samples from us but we couldn't
     * fill in time — hardware sent silence (or PA's last samples)
     * for that callback. Not data-corrupting like the input side
     * (no garbage flows back into our DSP), but it's the signal
     * that the audio thread missed the playback deadline and the
     * listener is hearing a gap. Log at WARN rate-limited to once
     * per second so heavy stuttering doesn't flood the journal. */
    if (fl & paOutputUnderflow) {
        static uint64_t last_log_us = 0;
        static unsigned underflow_count = 0;
        underflow_count++;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t now_us = (uint64_t)now.tv_sec * 1000000u +
                          (uint64_t)now.tv_nsec / 1000u;
        if (now_us - last_log_us > 1000000u) {
            KERCHUNK_LOG_W(LOG_MOD,
                "PA output underflow: %u events in last second, n=%lu — "
                "audio thread missed the playback deadline",
                underflow_count, n);
            last_log_us = now_us;
            underflow_count = 0;
        }
    }

    if (g_hw_rate == g_rate) {
        size_t got = kerchunk_audio_ring_read(&g_play_ring, dst, n);
        if (got < n)
            memset(dst + got, 0, (n - got) * sizeof(int16_t));
    } else {
        /* Upsample g_rate → hw_rate with persistent state.
         * Read 8kHz samples from ring, interpolate to hw_rate output.
         * g_play_prev holds the last sample for inter-callback continuity. */
        double step = (double)g_rate / (double)g_hw_rate;

        for (unsigned long i = 0; i < n; i++) {
            /* Read new source samples as needed */
            while (g_play_resample_pos >= 1.0) {
                g_play_prev = g_play_cur;
                int16_t s;
                if (kerchunk_audio_ring_read(&g_play_ring, &s, 1) == 1)
                    g_play_cur = s;
                else
                    g_play_cur = 0;  /* Underrun — silence */
                g_play_resample_pos -= 1.0;
            }

            /* Linear interpolation between prev and cur */
            double frac = g_play_resample_pos;
            dst[i] = (int16_t)((1.0 - frac) * g_play_prev + frac * g_play_cur);
            g_play_resample_pos += step;
        }
    }
    return paContinue;
}

/* ── Device lookup ── */

static PaDeviceIndex find_dev(const char *name, int is_input)
{
    if (!name || strcmp(name, "default") == 0)
        return is_input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();

    char *end;
    long idx = strtol(name, &end, 10);
    if (*end == '\0' && idx >= 0 && idx < Pa_GetDeviceCount())
        return (PaDeviceIndex)idx;

    int cnt = Pa_GetDeviceCount();
    for (int i = 0; i < cnt; i++) {
        const PaDeviceInfo *d = Pa_GetDeviceInfo(i);
        if (!d) continue;
        if (is_input  && d->maxInputChannels  < 1) continue;
        if (!is_input && d->maxOutputChannels < 1) continue;
        if (strstr(d->name, name)) return i;
    }

    KERCHUNK_LOG_W(LOG_MOD, "device '%s' not found, using default", name);
    return is_input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
}

/* ── Public API ── */

int kerchunk_audio_init(const kerchunk_audio_config_t *cfg)
{
    if (!cfg) return -1;

    g_rate         = cfg->sample_rate > 0 ? cfg->sample_rate : 48000;
    if (g_rate != 8000 && g_rate != 16000 && g_rate != 32000 && g_rate != 48000) {
        KERCHUNK_LOG_W(LOG_MOD, "invalid sample_rate %d, falling back to 48000", g_rate);
        g_rate = 48000;
    }
    g_preemph_on   = cfg->preemphasis;
    g_preemph_alpha = cfg->preemphasis_alpha;
    g_preemph_prev = 0;

    kerchunk_audio_ring_init(&g_cap_ring);
    kerchunk_audio_ring_init(&g_play_ring);

#ifdef __linux__
    /* Set ALSA mixer levels from config before opening the stream.
     * USB audio devices (CM119) reset mixer state on every plug/enumeration,
     * so we must configure volume/AGC at startup. */
    {
        snd_mixer_t *mixer = NULL;
        if (snd_mixer_open(&mixer, 0) == 0) {
            /* Find the card from the PortAudio device name.
             * For USB devices, try hw:0 first (most common). */
            if (snd_mixer_attach(mixer, "hw:0") == 0 &&
                snd_mixer_selem_register(mixer, NULL, NULL) == 0 &&
                snd_mixer_load(mixer) == 0) {

                snd_mixer_elem_t *elem;
                for (elem = snd_mixer_first_elem(mixer); elem;
                     elem = snd_mixer_elem_next(elem)) {
                    const char *name = snd_mixer_selem_get_name(elem);
                    if (!name) continue;

                    /* Speaker Playback Volume */
                    if (cfg->speaker_volume >= 0 &&
                        strcmp(name, "Speaker") == 0 &&
                        snd_mixer_selem_has_playback_volume(elem)) {
                        long min, max;
                        snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
                        long vol = cfg->speaker_volume;
                        if (vol > max) vol = max;
                        snd_mixer_selem_set_playback_volume_all(elem, vol);
                        KERCHUNK_LOG_I(LOG_MOD, "mixer: Speaker volume = %ld/%ld",
                                       vol, max);
                    }

                    /* Mic Capture Volume */
                    if (cfg->mic_volume >= 0 &&
                        strcmp(name, "Mic") == 0 &&
                        snd_mixer_selem_has_capture_volume(elem)) {
                        long min, max;
                        snd_mixer_selem_get_capture_volume_range(elem, &min, &max);
                        long vol = cfg->mic_volume;
                        if (vol > max) vol = max;
                        snd_mixer_selem_set_capture_volume_all(elem, vol);
                        KERCHUNK_LOG_I(LOG_MOD, "mixer: Mic Capture volume = %ld/%ld",
                                       vol, max);
                    }

                    /* Auto Gain Control */
                    if (cfg->agc >= 0 &&
                        strcmp(name, "Auto Gain Control") == 0 &&
                        snd_mixer_selem_has_playback_switch(elem)) {
                        snd_mixer_selem_set_playback_switch_all(elem, cfg->agc);
                        KERCHUNK_LOG_I(LOG_MOD, "mixer: AGC = %s",
                                       cfg->agc ? "on" : "off");
                    }
                }
            } else {
                KERCHUNK_LOG_W(LOG_MOD, "mixer: could not attach to hw:0");
            }
            snd_mixer_close(mixer);
        }
    }
#endif

    /* Suppress ALSA/JACK stderr noise during PortAudio init.
     * On headless Linux, Pa_Initialize() triggers ALSA's device
     * enumeration which dumps dozens of "Unknown PCM" errors to
     * stderr. Redirect stderr to /dev/null during init. */
    int saved_stderr = -1;
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        saved_stderr = dup(STDERR_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    PaError e = Pa_Initialize();

    if (saved_stderr >= 0) {
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }

    if (e != paNoError) {
        KERCHUNK_LOG_E(LOG_MOD, "Pa_Initialize: %s", Pa_GetErrorText(e));
        return -1;
    }
    g_pa_init = 1;

    PaDeviceIndex ci = find_dev(cfg->capture_device, 1);
    PaDeviceIndex pi = find_dev(cfg->playback_device, 0);
    if (ci == paNoDevice || pi == paNoDevice) {
        KERCHUNK_LOG_W(LOG_MOD, "no audio devices found — running without audio "
                     "(CLI and modules still functional)");
        Pa_Terminate(); g_pa_init = 0;
        return -1;
    }

    const PaDeviceInfo *cd = Pa_GetDeviceInfo(ci);
    const PaDeviceInfo *pd = Pa_GetDeviceInfo(pi);

    /* Hardware sample rate selection.
     *
     * Many USB devices (including radio interfaces) claim to support 8kHz
     * but produce poor audio because their driver does bad internal SRC.
     * Best practice: always open at a high native rate and resample in
     * software where we control quality.
     *
     * Config options:
     *   hw_rate = 48000   Force this rate (recommended for USB devices)
     *   hw_rate = 0       Auto-detect (try target rate, fall back to native)
     *   (not set)         Same as 0
     */
    g_hw_rate = cfg->hw_rate > 0 ? cfg->hw_rate : g_rate;
    g_cap_resample_pos = 0.0;
    g_play_resample_pos = 0.0;

    PaStreamParameters cp = {
        .device           = ci,
        .channelCount     = 1,
        .sampleFormat     = paInt16,
        .suggestedLatency = cd->defaultHighInputLatency,
    };

    if (cfg->hw_rate <= 0) {
        /* Auto: check if target rate is supported */
        e = Pa_IsFormatSupported(&cp, NULL, g_rate);
        if (e != paFormatIsSupported) {
            g_hw_rate = (int)cd->defaultSampleRate;
            KERCHUNK_LOG_W(LOG_MOD, "device does not support %d Hz, "
                         "using %d Hz with software resampling",
                         g_rate, g_hw_rate);
        }
    } else {
        KERCHUNK_LOG_I(LOG_MOD, "hw_rate forced to %d Hz", g_hw_rate);
    }

    /* Let PortAudio/ALSA negotiate optimal buffer sizes.
     * Fixed frame counts can cause NULL buffer crashes in PortAudio's
     * ALSA adapter when the negotiated size doesn't match. */
    unsigned long hw_frames = paFramesPerBufferUnspecified;

    g_duplex_stream = NULL;

    if (ci == pi) {
        /* Same device — try full-duplex stream (shared clock, no drift) */
        PaStreamParameters pp = {
            .device           = pi,
            .channelCount     = 1,
            .sampleFormat     = paInt16,
            .suggestedLatency = pd->defaultHighOutputLatency,
        };

        e = Pa_OpenStream(&g_duplex_stream, &cp, &pp, g_hw_rate, hw_frames,
                          paClipOff, duplex_cb, NULL);
        if (e == paNoError) {
            Pa_StartStream(g_duplex_stream);
            g_available = 1;

            KERCHUNK_LOG_I(LOG_MOD, "audio: duplex='%s' target=%dHz hw=%dHz%s ring=%u",
                         cd->name, g_rate, g_hw_rate,
                         g_hw_rate != g_rate ? " (resampling)" : "",
                         KERCHUNK_AUDIO_RING_SIZE);
            return 0;
        }

        /* Duplex failed — fall back to separate streams */
        KERCHUNK_LOG_W(LOG_MOD, "duplex open failed (%s), trying separate streams",
                     Pa_GetErrorText(e));
        g_duplex_stream = NULL;
    }

    /* Separate streams (different devices, or duplex fallback) */

    /* Capture stream */
    e = Pa_OpenStream(&g_cap_stream, &cp, NULL, g_hw_rate, hw_frames,
                      paClipOff, cap_cb, NULL);
    if (e != paNoError) {
        KERCHUNK_LOG_E(LOG_MOD, "capture open: %s", Pa_GetErrorText(e));
        Pa_Terminate(); g_pa_init = 0;
        return -1;
    }

    /* Playback stream */
    PaStreamParameters pp = {
        .device           = pi,
        .channelCount     = 1,
        .sampleFormat     = paInt16,
        .suggestedLatency = pd->defaultHighOutputLatency,
    };
    e = Pa_OpenStream(&g_play_stream, NULL, &pp, g_hw_rate, hw_frames,
                      paClipOff, play_cb, NULL);
    if (e != paNoError) {
        KERCHUNK_LOG_E(LOG_MOD, "playback open: %s", Pa_GetErrorText(e));
        Pa_CloseStream(g_cap_stream); g_cap_stream = NULL;
        Pa_Terminate(); g_pa_init = 0;
        return -1;
    }

    Pa_StartStream(g_cap_stream);
    Pa_StartStream(g_play_stream);
    g_available = 1;

    KERCHUNK_LOG_I(LOG_MOD, "audio: capture='%s' playback='%s' "
                 "target=%dHz hw=%dHz%s ring=%u",
                 cd->name, pd->name, g_rate, g_hw_rate,
                 g_hw_rate != g_rate ? " (resampling)" : "",
                 KERCHUNK_AUDIO_RING_SIZE);
    return 0;
}

void kerchunk_audio_shutdown(void)
{
    if (g_duplex_stream) { Pa_StopStream(g_duplex_stream); Pa_CloseStream(g_duplex_stream); g_duplex_stream = NULL; }
    if (g_cap_stream)    { Pa_StopStream(g_cap_stream);    Pa_CloseStream(g_cap_stream);    g_cap_stream    = NULL; }
    if (g_play_stream)   { Pa_StopStream(g_play_stream);   Pa_CloseStream(g_play_stream);   g_play_stream   = NULL; }
    if (g_pa_init)       { Pa_Terminate(); g_pa_init = 0; }
    g_available = 0;
}

int kerchunk_audio_capture(int16_t *buf, size_t n)
{
    if (!buf) return -1;
    size_t got = kerchunk_audio_ring_read(&g_cap_ring, buf, n);
    if (got == 0) return 0;  /* Nothing available — caller should skip */
    if (got < n) {
        /* Partial read: instead of zero-padding the tail (which
         * injects a hard silence frame and breaks the DTMF decoder's
         * 2-block hysteresis lock-on, plus produces an audible click
         * in the relay/web paths), repeat samples from the tail of
         * the last good frame. The decoder sees "tone continues" and
         * the audio path stays continuous.
         *
         * Historical note: the pre-refactor code had a slightly
         * different fallback here — when g_cap_last_n was smaller
         * than the missing count (i.e. `miss`), it zero-filled
         * rather than tiling. In practice g_cap_last_n is always
         * equal to frame_samples (from the save path below) or 0,
         * and `miss` is always ≤ frame_samples, so the old path
         * only hit its silence-fallback when g_cap_last_n == 0 —
         * which kerchunk_audio_repeat_fill() also zero-fills via
         * its last_n==0 branch. No practical behavior change. */
        kerchunk_audio_repeat_fill(buf + got, n - got,
                                   g_cap_last, g_cap_last_n);
    } else if (n <= KERCHUNK_MAX_FRAME_SAMPLES) {
        /* Only save as last-good on a COMPLETE ring read. Saving a
         * partially-repeated buffer would make subsequent under-runs
         * repeat-already-repeated samples, compounding distortion. */
        memcpy(g_cap_last, buf, n * sizeof(int16_t));
        g_cap_last_n = n;
    }
    if (g_preemph_on)
        kerchunk_audio_preemphasis(buf, n, g_preemph_alpha);
    return (int)n;
}

void kerchunk_audio_capture_repeat_last(int16_t *buf, size_t n)
{
    /* Thin wrapper over the pure helper in kerchunk_audio_ring.c —
     * the helper gets unit-tested against scripted (last_buf, last_n)
     * states that would be hard to reach through the PA callback. */
    kerchunk_audio_repeat_fill(buf, n, g_cap_last, g_cap_last_n);
}

int kerchunk_audio_playback(const int16_t *buf, size_t n)
{
    if (!buf) return -1;
    kerchunk_audio_ring_write(&g_play_ring, buf, n);
    return (int)n;
}

size_t kerchunk_audio_playback_writable(void) { return kerchunk_audio_ring_writable(&g_play_ring); }
size_t kerchunk_audio_playback_pending(void)  { return kerchunk_audio_ring_readable(&g_play_ring); }
size_t kerchunk_audio_capture_pending(void)   { return kerchunk_audio_ring_readable(&g_cap_ring); }

int kerchunk_audio_available(void) { return g_available; }

int kerchunk_audio_get_rate(void) { return g_rate; }

void kerchunk_audio_list_devices(void)
{
    int need_term = 0;
    if (!g_pa_init) {
        int sv = -1, dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { sv = dup(STDERR_FILENO); dup2(dn, STDERR_FILENO); close(dn); }
        PaError err = Pa_Initialize();
        if (sv >= 0) { dup2(sv, STDERR_FILENO); close(sv); }
        if (err != paNoError) { fprintf(stderr, "Pa_Initialize failed\n"); return; }
        need_term = 1;
    }

    int cnt = Pa_GetDeviceCount();
    fprintf(stderr, "Audio devices (%d):\n", cnt);
    for (int i = 0; i < cnt; i++) {
        const PaDeviceInfo *d = Pa_GetDeviceInfo(i);
        if (!d) continue;
        const PaHostApiInfo *a = Pa_GetHostApiInfo(d->hostApi);
        fprintf(stderr, "  [%d] %s (%s) in=%d out=%d\n",
                i, d->name, a ? a->name : "?",
                d->maxInputChannels, d->maxOutputChannels);
    }
    fprintf(stderr, "  Default input:  [%d]\n", Pa_GetDefaultInputDevice());
    fprintf(stderr, "  Default output: [%d]\n", Pa_GetDefaultOutputDevice());

    if (need_term) Pa_Terminate();
}
