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

/* ── Lock-free SPSC ring buffer ── */

#define RING_SIZE 262144        /* ~5.5s at 48 kHz, must be power of 2 */
#define RING_MASK (RING_SIZE - 1)

typedef struct {
    int16_t       buf[RING_SIZE];
    atomic_size_t head;         /* producer write position */
    atomic_size_t tail;         /* consumer read position */
} ring_t;

static void ring_init(ring_t *r)
{
    memset(r->buf, 0, sizeof(r->buf));
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
}

static size_t ring_readable(const ring_t *r)
{
    return (atomic_load(&r->head) - atomic_load(&r->tail)) & RING_MASK;
}

static size_t ring_writable(const ring_t *r)
{
    return RING_SIZE - 1 - ring_readable(r);
}

static size_t ring_write(ring_t *r, const int16_t *src, size_t n)
{
    size_t space = ring_writable(r);
    if (n > space) n = space;
    size_t h = atomic_load(&r->head);
    for (size_t i = 0; i < n; i++)
        r->buf[(h + i) & RING_MASK] = src[i];
    atomic_store(&r->head, (h + n) & RING_MASK);
    return n;
}

static size_t ring_read(ring_t *r, int16_t *dst, size_t n)
{
    size_t avail = ring_readable(r);
    if (n > avail) n = avail;
    size_t t = atomic_load(&r->tail);
    for (size_t i = 0; i < n; i++)
        dst[i] = r->buf[(t + i) & RING_MASK];
    atomic_store(&r->tail, (t + n) & RING_MASK);
    return n;
}

/* ── PortAudio state ── */

static PaStream *g_cap_stream;
static PaStream *g_play_stream;
static ring_t    g_cap_ring;
static ring_t    g_play_ring;
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
    (void)ti; (void)fl; (void)ud;

    /* Capture side */
    if (in) {
        const int16_t *src = (const int16_t *)in;
        if (g_hw_rate == g_rate) {
            ring_write(&g_cap_ring, src, n);
        } else {
            double step = (double)g_hw_rate / (double)g_rate;
            int16_t tmp[512];
            size_t out_i = 0;
            while (g_cap_resample_pos < (double)n) {
                size_t idx = (size_t)g_cap_resample_pos;
                double frac = g_cap_resample_pos - (double)idx;
                int16_t s0 = (idx < n) ? src[idx] : 0;
                int16_t s1 = (idx + 1 < n) ? src[idx + 1] : s0;
                tmp[out_i++] = (int16_t)(s0 + frac * (s1 - s0));
                if (out_i == 512) { ring_write(&g_cap_ring, tmp, 512); out_i = 0; }
                g_cap_resample_pos += step;
            }
            g_cap_resample_pos -= (double)n;
            if (out_i > 0) ring_write(&g_cap_ring, tmp, out_i);
        }
    }

    /* Playback side */
    if (out) {
        int16_t *dst = (int16_t *)out;
        if (g_hw_rate == g_rate) {
            size_t got = ring_read(&g_play_ring, dst, n);
            if (got < n) memset(dst + got, 0, (n - got) * sizeof(int16_t));
        } else {
            double step = (double)g_rate / (double)g_hw_rate;
            for (unsigned long i = 0; i < n; i++) {
                while (g_play_resample_pos >= 1.0) {
                    g_play_prev = g_play_cur;
                    int16_t s;
                    if (ring_read(&g_play_ring, &s, 1) == 1) g_play_cur = s;
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

    /* PortAudio sets paInputUnderflow when the audio device couldn't
     * deliver fresh data and PA filled the callback buffer with zeros.
     * Pushing those zeros into the capture ring puts a mid-stream
     * silence dropout in everything downstream — recordings get
     * 21 ms gaps, the relay clicks, the DTMF decoder's hysteresis
     * breaks. Drop the buffer and let the audio thread's empty-ring
     * path repeat-last instead. Observed in practice on Pi 5 +
     * CM108AH USB audio: ~40% of an idle-period recording was these
     * 1024-sample zero frames. */
    if (fl & paInputUnderflow) {
        return paContinue;
    }

    const int16_t *src = (const int16_t *)in;

    if (g_hw_rate == g_rate) {
        ring_write(&g_cap_ring, src, n);
    } else {
        /* Downsample hw_rate → g_rate with persistent fractional position.
         * Maintains phase continuity across callback boundaries. */
        double step = (double)g_hw_rate / (double)g_rate;
        int16_t tmp[512];
        size_t out_i = 0;

        while (g_cap_resample_pos < (double)n) {
            size_t idx = (size_t)g_cap_resample_pos;
            double frac = g_cap_resample_pos - (double)idx;
            int16_t s0 = (idx < n) ? src[idx] : 0;
            int16_t s1 = (idx + 1 < n) ? src[idx + 1] : s0;
            tmp[out_i++] = (int16_t)(s0 + frac * (s1 - s0));

            if (out_i == 512) {
                ring_write(&g_cap_ring, tmp, 512);
                out_i = 0;
            }
            g_cap_resample_pos += step;
        }
        g_cap_resample_pos -= (double)n;  /* Carry fractional remainder */

        if (out_i > 0)
            ring_write(&g_cap_ring, tmp, out_i);
    }
    return paContinue;
}

static int play_cb(const void *in, void *out, unsigned long n,
                   const PaStreamCallbackTimeInfo *ti,
                   PaStreamCallbackFlags fl, void *ud)
{
    (void)in; (void)ti; (void)fl; (void)ud;
    int16_t *dst = (int16_t *)out;

    if (g_hw_rate == g_rate) {
        size_t got = ring_read(&g_play_ring, dst, n);
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
                if (ring_read(&g_play_ring, &s, 1) == 1)
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

    ring_init(&g_cap_ring);
    ring_init(&g_play_ring);

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

            KERCHUNK_LOG_I(LOG_MOD, "audio: duplex='%s' target=%dHz hw=%dHz%s ring=%d",
                         cd->name, g_rate, g_hw_rate,
                         g_hw_rate != g_rate ? " (resampling)" : "",
                         RING_SIZE);
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
                 "target=%dHz hw=%dHz%s ring=%d",
                 cd->name, pd->name, g_rate, g_hw_rate,
                 g_hw_rate != g_rate ? " (resampling)" : "",
                 RING_SIZE);
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
    size_t got = ring_read(&g_cap_ring, buf, n);
    if (got == 0) return 0;  /* Nothing available — caller should skip */
    if (got < n) {
        /* Partial read: instead of zero-padding the tail (which
         * injects a hard silence frame and breaks the DTMF decoder's
         * 2-block hysteresis lock-on, plus produces an audible click
         * in the relay/web paths), repeat samples from the tail of
         * the last good frame. The decoder sees "tone continues" and
         * the audio path stays continuous. */
        size_t miss = n - got;
        if (g_cap_last_n >= miss) {
            memcpy(buf + got, g_cap_last + (g_cap_last_n - miss),
                   miss * sizeof(int16_t));
        } else {
            /* No prior frame yet — fall back to silence */
            memset(buf + got, 0, miss * sizeof(int16_t));
        }
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
    if (!buf || n == 0) return;
    if (g_cap_last_n == 0) {
        memset(buf, 0, n * sizeof(int16_t));
        return;
    }
    if (g_cap_last_n >= n) {
        memcpy(buf, g_cap_last + (g_cap_last_n - n),
               n * sizeof(int16_t));
        return;
    }
    /* n exceeds what we have saved — tile the last frame across buf */
    size_t pos = 0;
    while (pos < n) {
        size_t chunk = (n - pos) < g_cap_last_n ? (n - pos) : g_cap_last_n;
        memcpy(buf + pos, g_cap_last, chunk * sizeof(int16_t));
        pos += chunk;
    }
}

int kerchunk_audio_playback(const int16_t *buf, size_t n)
{
    if (!buf) return -1;
    ring_write(&g_play_ring, buf, n);
    return (int)n;
}

size_t kerchunk_audio_playback_writable(void) { return ring_writable(&g_play_ring); }
size_t kerchunk_audio_playback_pending(void)  { return ring_readable(&g_play_ring); }
size_t kerchunk_audio_capture_pending(void)   { return ring_readable(&g_cap_ring); }

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
