/*
 * kerchunk_diag.c — Standalone PTT + audio diagnostic tool
 *
 * Tests the full TX chain independently of kerchunkd:
 *   1. HID PTT assert/release via hidraw
 *   2. PortAudio tone generation through playback device
 *   3. Combined test: PTT + tone (verifies radio keys and transmits)
 *
 * Usage:
 *   kerchunk-diag [options]
 *     -d <hidraw>    HID device (default: /dev/rimlite)
 *     -p <pin>       PTT GPIO number 0-7 (default: 2)
 *     -a <device>    Audio device name or index (default: system default)
 *     -r <rate>      Hardware sample rate (default: 48000)
 *     -f <freq>      Test tone frequency in Hz (default: 1000)
 *     -v <0-100>     Tone volume percent (default: 50)
 *     -t <secs>      Tone duration in seconds (default: 3)
 *     -T             PTT-only test (no audio)
 *     -A             Audio-only test (no PTT)
 *     -C             COR read test (poll COR state)
 *     -b <bit>       COR GPIO number 0-7 (default: 3)
 *     -l             Invert COR polarity (active low)
 *     -L             List audio devices and exit
 *     -h             Help
 *
 * Build:
 *   gcc -o kerchunk-diag tools/kerchunk_diag.c -lportaudio -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <portaudio.h>

#ifdef __linux__
#include <linux/hidraw.h>
#include <sys/ioctl.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── HID PTT ── */

static int hid_open(const char *device)
{
    int fd = open(device, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "ERROR: open %s: %s\n", device, strerror(errno));
        return -1;
    }

#ifdef __linux__
    struct hidraw_devinfo info;
    if (ioctl(fd, HIDIOCGRAWINFO, &info) == 0) {
        printf("  HID device: %s (vendor=%04x product=%04x)\n",
               device, info.vendor, info.product);
    }
#endif
    return fd;
}

static int hid_set_ptt(int fd, int ptt_pin, int active)
{
    unsigned char io[5] = {0};
    io[2] = active ? (unsigned char)(1 << ptt_pin) : 0;
    io[3] = (unsigned char)(1 << ptt_pin);

    ssize_t n = write(fd, io, sizeof(io));
    if (n < 0) {
        fprintf(stderr, "ERROR: HID write failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int hid_read_cor(int fd, int cor_bit, int active_low)
{
    unsigned char buf[8];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0)
        return -1;

    int raw = (buf[0] >> cor_bit) & 1;
    return active_low ? !raw : raw;
}

/* ── Audio tone generator ── */

typedef struct {
    double phase;
    double phase_inc;
    double amplitude;
    int    samples_left;
} tone_state_t;

static int tone_cb(const void *in, void *out, unsigned long frames,
                   const PaStreamCallbackTimeInfo *ti,
                   PaStreamCallbackFlags fl, void *ud)
{
    (void)in; (void)ti; (void)fl;
    tone_state_t *s = (tone_state_t *)ud;
    int16_t *dst = (int16_t *)out;

    for (unsigned long i = 0; i < frames; i++) {
        if (s->samples_left <= 0) {
            dst[i] = 0;
        } else {
            dst[i] = (int16_t)(s->amplitude * sin(s->phase));
            s->phase += s->phase_inc;
            if (s->phase > 2.0 * M_PI)
                s->phase -= 2.0 * M_PI;
            s->samples_left--;
        }
    }

    return (s->samples_left <= 0) ? paComplete : paContinue;
}

static PaDeviceIndex find_device(const char *name)
{
    if (!name)
        return Pa_GetDefaultOutputDevice();

    char *end;
    long idx = strtol(name, &end, 10);
    if (*end == '\0' && idx >= 0 && idx < Pa_GetDeviceCount())
        return (PaDeviceIndex)idx;

    int cnt = Pa_GetDeviceCount();
    for (int i = 0; i < cnt; i++) {
        const PaDeviceInfo *d = Pa_GetDeviceInfo(i);
        if (d && d->maxOutputChannels >= 1 && strstr(d->name, name))
            return i;
    }

    fprintf(stderr, "WARNING: device '%s' not found, using default\n", name);
    return Pa_GetDefaultOutputDevice();
}

static void list_devices(void)
{
    int cnt = Pa_GetDeviceCount();
    printf("Audio devices (%d):\n", cnt);
    for (int i = 0; i < cnt; i++) {
        const PaDeviceInfo *d = Pa_GetDeviceInfo(i);
        if (!d) continue;
        const PaHostApiInfo *a = Pa_GetHostApiInfo(d->hostApi);
        printf("  [%d] %s (%s) in=%d out=%d rate=%.0f\n",
               i, d->name, a ? a->name : "?",
               d->maxInputChannels, d->maxOutputChannels,
               d->defaultSampleRate);
    }
    printf("  Default input:  [%d]\n", Pa_GetDefaultInputDevice());
    printf("  Default output: [%d]\n", Pa_GetDefaultOutputDevice());
}

/* ── Main ── */

static volatile int g_running = 1;

static void sighandler(int sig) { (void)sig; g_running = 0; }

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -d <hidraw>   HID device (default: /dev/rimlite)\n"
        "  -p <pin>      PTT GPIO number 0-7 (default: 2)\n"
        "  -a <device>   Audio device name or index\n"
        "  -r <rate>     Hardware sample rate (default: 48000)\n"
        "  -f <freq>     Tone frequency Hz (default: 1000)\n"
        "  -v <0-100>    Volume percent (default: 50)\n"
        "  -t <secs>     Tone duration seconds (default: 3)\n"
        "  -T            PTT-only test (no audio)\n"
        "  -A            Audio-only test (no PTT)\n"
        "  -C            COR read test (poll and display)\n"
        "  -b <bit>      COR GPIO number 0-7 (default: 3)\n"
        "  -l            COR active low\n"
        "  -L            List audio devices\n"
        "  -h            Help\n", prog);
}

int main(int argc, char *argv[])
{
    const char *hid_device = "/dev/rimlite";
    int ptt_pin = 2;  /* GPIO2 = PTT on CM119/RIM-Lite */
    const char *audio_device = NULL;
    int hw_rate = 48000;
    int tone_freq = 1000;
    int volume = 50;
    int duration = 3;
    int ptt_only = 0;
    int audio_only = 0;
    int cor_test = 0;
    int cor_bit = 3;
    int cor_active_low = 0;
    int list_only = 0;

    int opt;
    while ((opt = getopt(argc, argv, "d:p:a:r:f:v:t:TACb:lLh")) != -1) {
        switch (opt) {
        case 'd': hid_device = optarg; break;
        case 'p': ptt_pin = atoi(optarg); break;
        case 'a': audio_device = optarg; break;
        case 'r': hw_rate = atoi(optarg); break;
        case 'f': tone_freq = atoi(optarg); break;
        case 'v': volume = atoi(optarg); break;
        case 't': duration = atoi(optarg); break;
        case 'T': ptt_only = 1; break;
        case 'A': audio_only = 1; break;
        case 'C': cor_test = 1; break;
        case 'b': cor_bit = atoi(optarg); break;
        case 'l': cor_active_low = 1; break;
        case 'L': list_only = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }

    if (ptt_pin < 0 || ptt_pin > 7) {
        fprintf(stderr, "ERROR: PTT GPIO must be 0-7\n");
        return 1;
    }
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* ── COR test mode ── */
    if (cor_test) {
        int fd = hid_open(hid_device);
        if (fd < 0) return 1;

        printf("COR test: dumping raw HID reports from %s — Ctrl+C to stop\n",
               hid_device);
        printf("  Key your radio and watch which bits change.\n\n");

        while (g_running) {
            unsigned char buf[8] = {0};
            ssize_t n = read(fd, buf, sizeof(buf));
            if (n > 0) {
                printf("  HID [%zd bytes]:", n);
                for (ssize_t i = 0; i < n; i++)
                    printf(" %02x", buf[i]);
                printf("  | byte0 bits: ");
                for (int b = 7; b >= 0; b--)
                    printf("%d", (buf[0] >> b) & 1);
                printf("  | GPIO0=%d GPIO1=%d GPIO2=%d GPIO3=%d",
                       buf[0] & 1, (buf[0] >> 1) & 1,
                       (buf[0] >> 2) & 1, (buf[0] >> 3) & 1);
                printf("\n");
            }
            usleep(20000);
        }
        close(fd);
        return 0;
    }

    /* ── List devices ── */
    if (list_only) {
        PaError e = Pa_Initialize();
        if (e != paNoError) {
            fprintf(stderr, "Pa_Initialize: %s\n", Pa_GetErrorText(e));
            return 1;
        }
        list_devices();
        Pa_Terminate();
        return 0;
    }

    /* ── PTT-only test ── */
    if (ptt_only) {
        int fd = hid_open(hid_device);
        if (fd < 0) return 1;

        printf("PTT test: asserting GPIO%d on %s for %d seconds...\n",
               ptt_pin, hid_device, duration);

        if (hid_set_ptt(fd, ptt_pin, 1) < 0) {
            close(fd);
            return 1;
        }
        printf("  PTT: ON\n");

        for (int i = 0; i < duration && g_running; i++) {
            sleep(1);
            printf("  %d/%d seconds...\n", i + 1, duration);
        }

        hid_set_ptt(fd, ptt_pin, 0);
        printf("  PTT: OFF\n");
        close(fd);
        printf("DONE. Did your radio key up? Check the TX indicator.\n");
        return 0;
    }

    /* ── Audio + PTT test ── */
    printf("=== kerchunk diagnostic ===\n");
    printf("  HID device:  %s\n", hid_device);
    printf("  PTT:         GPIO%d\n", ptt_pin);
    printf("  Tone:        %d Hz, %d%% volume, %d seconds\n",
           tone_freq, volume, duration);
    printf("  Sample rate: %d Hz\n", hw_rate);
    printf("\n");

    /* Open HID */
    int hid_fd = -1;
    if (!audio_only) {
        hid_fd = hid_open(hid_device);
        if (hid_fd < 0)
            return 1;
    }

    /* Init PortAudio */
    PaError e = Pa_Initialize();
    if (e != paNoError) {
        fprintf(stderr, "ERROR: Pa_Initialize: %s\n", Pa_GetErrorText(e));
        if (hid_fd >= 0) close(hid_fd);
        return 1;
    }

    PaDeviceIndex pi = find_device(audio_device);
    if (pi == paNoDevice) {
        fprintf(stderr, "ERROR: no output device found\n");
        Pa_Terminate();
        if (hid_fd >= 0) close(hid_fd);
        return 1;
    }

    const PaDeviceInfo *pd = Pa_GetDeviceInfo(pi);
    printf("  Audio out:   [%d] %s\n\n", pi, pd->name);

    /* Setup tone */
    double amp = (32767.0 * volume) / 100.0;
    tone_state_t tone = {
        .phase = 0.0,
        .phase_inc = (2.0 * M_PI * tone_freq) / hw_rate,
        .amplitude = amp,
        .samples_left = hw_rate * duration,
    };

    PaStreamParameters pp = {
        .device = pi,
        .channelCount = 1,
        .sampleFormat = paInt16,
        .suggestedLatency = pd->defaultHighOutputLatency,
    };

    PaStream *stream = NULL;
    e = Pa_OpenStream(&stream, NULL, &pp, hw_rate,
                      paFramesPerBufferUnspecified, paClipOff,
                      tone_cb, &tone);
    if (e != paNoError) {
        fprintf(stderr, "ERROR: Pa_OpenStream: %s\n", Pa_GetErrorText(e));
        Pa_Terminate();
        if (hid_fd >= 0) close(hid_fd);
        return 1;
    }

    /* Step 1: Assert PTT */
    if (hid_fd >= 0) {
        printf("[1] Asserting PTT...\n");
        if (hid_set_ptt(hid_fd, ptt_pin, 1) < 0) {
            Pa_CloseStream(stream);
            Pa_Terminate();
            close(hid_fd);
            return 1;
        }
        printf("    PTT: ON — waiting 200ms for TX stabilization\n");
        usleep(200000);
    }

    /* Step 2: Play tone */
    printf("[2] Playing %d Hz tone at %d%% for %d seconds...\n",
           tone_freq, volume, duration);

    e = Pa_StartStream(stream);
    if (e != paNoError) {
        fprintf(stderr, "ERROR: Pa_StartStream: %s\n", Pa_GetErrorText(e));
        if (hid_fd >= 0) { hid_set_ptt(hid_fd, ptt_pin, 0); close(hid_fd); }
        Pa_CloseStream(stream);
        Pa_Terminate();
        return 1;
    }

    /* Wait for tone to complete */
    while (Pa_IsStreamActive(stream) == 1 && g_running)
        usleep(100000);

    Pa_StopStream(stream);
    Pa_CloseStream(stream);

    /* Step 3: Release PTT */
    if (hid_fd >= 0) {
        printf("[3] Releasing PTT...\n");
        usleep(200000);  /* tx_tail */
        hid_set_ptt(hid_fd, ptt_pin, 0);
        printf("    PTT: OFF\n");
        close(hid_fd);
    }

    Pa_Terminate();

    printf("\n=== Results ===\n");
    printf("If you heard the tone on a receiver, TX chain is working.\n");
    printf("If the radio keyed but no audio, check:\n");
    printf("  - ALSA mixer: amixer -c0 contents\n");
    printf("  - Speaker volume in kerchunk.conf [audio] section\n");
    printf("  - Try higher volume: %s -v 100\n", argv[0]);
    printf("If the radio did NOT key up, check:\n");
    printf("  - HID device: ls -la %s\n", hid_device);
    printf("  - PTT pin: try -p 3 or -p 4\n");
    printf("  - Permissions: run as root or add user to plugdev\n");

    return 0;
}
