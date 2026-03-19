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
#include "kerchunk_log.h"
#include <portaudio.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>
#include <fcntl.h>

#define LOG_MOD "audio"

/* ── Pre-emphasis filter state ── */

static int     g_preemph_on;
static float   g_preemph_alpha;
static int16_t g_preemph_prev;

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

#define RING_SIZE 16384         /* ~2s at 8 kHz, must be power of 2 */
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
                    else g_play_cur = 0;
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
    (void)out; (void)ti; (void)fl; (void)ud;
    if (!in) return paContinue;

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

    g_rate         = cfg->sample_rate > 0 ? cfg->sample_rate : 8000;
    g_preemph_on   = cfg->preemphasis;
    g_preemph_alpha = cfg->preemphasis_alpha;
    g_preemph_prev = 0;

    ring_init(&g_cap_ring);
    ring_init(&g_play_ring);

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
    if (got < n)
        memset(buf + got, 0, (n - got) * sizeof(int16_t));
    got = n;  /* Report full frame — buffer is zero-padded */
    if (g_preemph_on)
        kerchunk_audio_preemphasis(buf, got, g_preemph_alpha);
    return (int)got;
}

int kerchunk_audio_playback(const int16_t *buf, size_t n)
{
    if (!buf) return -1;
    ring_write(&g_play_ring, buf, n);
    return (int)n;
}

size_t kerchunk_audio_playback_writable(void) { return ring_writable(&g_play_ring); }
size_t kerchunk_audio_playback_pending(void)  { return ring_readable(&g_play_ring); }

int kerchunk_audio_available(void) { return g_available; }

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
