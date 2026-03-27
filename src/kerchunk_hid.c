/*
 * kerchunk_hid.c — HID driver for RIM-Lite COR/PTT via hidraw
 *
 * CM119 USB audio chips expose GPIO bits via HID reports.
 * COR is read from an input bit, PTT is set via an output bit.
 */

#include "kerchunk_hid.h"
#include "kerchunk_log.h"
#include <string.h>
#include <errno.h>

#define LOG_MOD "hid"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

static int  g_fd = -1;
static char g_device[256];
static int  g_cor_bit;
static int  g_cor_active_low;
static int  g_ptt_bit;
static int  g_available;

/* Write CM108/CM119 GPIO via hidraw.
 * Buffer format (5 bytes):
 *   [0] = 0x00       (reserved)
 *   [1] = 0x00       (reserved)
 *   [2] = GPIO data  (pin values: 1=high, 0=low)
 *   [3] = GPIO mask  (direction: 1=output for each bit)
 *   [4] = 0x00       (SPDIF)
 * GPIO pin N maps to bit (N-1): GPIO3 → bit 2, GPIO4 → bit 3, etc.
 * MUST be 5 bytes — 4 bytes causes EPIPE on CM108/CM119. */
static int hid_write_gpio(int fd, int pin_bit, int state)
{
    unsigned char io[5] = {0};
    io[2] = state ? (unsigned char)(1 << pin_bit) : 0;  /* data: pin value */
    io[3] = (unsigned char)(1 << pin_bit);  /* mask: this pin is output */

    ssize_t n = write(fd, io, sizeof(io));
    if (n < 0)
        return -1;
    return 0;
}

int kerchunk_hid_init(const kerchunk_hid_config_t *cfg)
{
    if (!cfg || !cfg->device)
        return -1;

    g_fd = open(cfg->device, O_RDWR | O_NONBLOCK);
    if (g_fd < 0) {
        KERCHUNK_LOG_E(LOG_MOD, "open %s failed: %s",
                       cfg->device, strerror(errno));
        return -1;
    }

    snprintf(g_device, sizeof(g_device), "%s", cfg->device);
    g_cor_bit       = cfg->cor_bit;
    g_cor_active_low = cfg->cor_polarity;
    g_ptt_bit       = cfg->ptt_bit;
    g_available     = 1;

    /* Log HID device info for diagnostics */
    struct hidraw_devinfo info;
    if (ioctl(g_fd, HIDIOCGRAWINFO, &info) == 0) {
        KERCHUNK_LOG_I(LOG_MOD, "hid init: device=%s vendor=%04x product=%04x "
                     "cor_bit=%d ptt_pin=GPIO%d (bit %d)",
                     g_device, info.vendor, info.product,
                     g_cor_bit, g_ptt_bit, g_ptt_bit - 1);
    } else {
        KERCHUNK_LOG_I(LOG_MOD, "hid init: device=%s cor_bit=%d ptt_pin=GPIO%d",
                     g_device, g_cor_bit, g_ptt_bit);
    }

    /* Probe: write PTT off to verify device is responsive */
    if (hid_write_gpio(g_fd, g_ptt_bit - 1, 0) < 0)
        KERCHUNK_LOG_W(LOG_MOD, "GPIO probe write failed: %s "
                       "(PTT may not work)", strerror(errno));
    else
        KERCHUNK_LOG_I(LOG_MOD, "GPIO write OK (5-byte CM108/CM119 format)");

    return 0;
}

void kerchunk_hid_shutdown(void)
{
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    g_available = 0;
}

/* Attempt to reopen the device after USB disconnect/reconnect */
static int hid_reconnect(void)
{
    if (g_fd >= 0)
        close(g_fd);
    g_fd = open(g_device, O_RDWR | O_NONBLOCK);
    if (g_fd < 0) {
        g_available = 0;
        return -1;
    }
    g_available = 1;
    KERCHUNK_LOG_I(LOG_MOD, "reconnected to %s (fd=%d)", g_device, g_fd);
    return 0;
}

int kerchunk_hid_read_cor(void)
{
    if (g_fd < 0)
        return -1;

    unsigned char buf[4] = {0};
    ssize_t n = read(g_fd, buf, sizeof(buf));
    if (n < 0 && (errno == EPIPE || errno == ENODEV || errno == EIO)) {
        KERCHUNK_LOG_W(LOG_MOD, "cor read: %s, reconnecting", strerror(errno));
        if (hid_reconnect() < 0)
            return -1;
        n = read(g_fd, buf, sizeof(buf));
    }
    if (n < 1)
        return -1;  /* No data available (non-blocking) */

    int bit = (buf[0] >> g_cor_bit) & 1;
    return g_cor_active_low ? !bit : bit;
}

int kerchunk_hid_set_ptt(int active)
{
    if (g_fd < 0)
        return -1;

    /* GPIO pin N maps to bit (N-1) in the CM108/CM119 HID report */
    int pin_bit = g_ptt_bit - 1;

    int rc = hid_write_gpio(g_fd, pin_bit, active);
    if (rc < 0 && (errno == EPIPE || errno == ENODEV || errno == EIO)) {
        KERCHUNK_LOG_W(LOG_MOD, "ptt write: %s, reconnecting", strerror(errno));
        if (hid_reconnect() < 0)
            return -1;
        rc = hid_write_gpio(g_fd, pin_bit, active);
    }
    if (rc < 0) {
        KERCHUNK_LOG_E(LOG_MOD, "ptt write failed: %s (fd=%d, GPIO%d=%d)",
                       strerror(errno), g_fd, g_ptt_bit, active);
        return -1;
    }
    return 0;
}

int kerchunk_hid_available(void)
{
    return g_available;
}

#else /* Non-Linux stub */

static int g_stub_cor;
static int g_stub_ptt;

int kerchunk_hid_init(const kerchunk_hid_config_t *cfg)
{
    (void)cfg;
    g_stub_cor = 0;
    g_stub_ptt = 0;
    KERCHUNK_LOG_W(LOG_MOD, "hidraw not available on this platform (stub mode)");
    return 0;
}

void kerchunk_hid_shutdown(void) {}

int kerchunk_hid_read_cor(void)
{
    return g_stub_cor;
}

int kerchunk_hid_set_ptt(int active)
{
    g_stub_ptt = active;
    return 0;
}

int kerchunk_hid_available(void)
{
    return 0;
}

#endif
