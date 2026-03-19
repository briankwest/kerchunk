/*
 * kerchunk_hid.c — HID driver for RIM-Lite COR/PTT via hidraw
 *
 * CM119 USB audio chips expose GPIO bits via HID reports.
 * COR is read from an input bit, PTT is set via an output bit.
 */

#include "kerchunk_hid.h"
#include "kerchunk_log.h"
#include <string.h>

#define LOG_MOD "hid"

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

static int  g_fd = -1;
static int  g_cor_bit;
static int  g_cor_active_low;
static int  g_ptt_bit;
static int  g_available;

int kerchunk_hid_init(const kerchunk_hid_config_t *cfg)
{
    if (!cfg || !cfg->device)
        return -1;

    g_fd = open(cfg->device, O_RDWR | O_NONBLOCK);
    if (g_fd < 0) {
        KERCHUNK_LOG_E(LOG_MOD, "open %s failed", cfg->device);
        return -1;
    }

    g_cor_bit       = cfg->cor_bit;
    g_cor_active_low = cfg->cor_polarity;
    g_ptt_bit       = cfg->ptt_bit;
    g_available     = 1;

    KERCHUNK_LOG_I(LOG_MOD, "hid init: device=%s cor_bit=%d ptt_bit=%d",
                 cfg->device, g_cor_bit, g_ptt_bit);
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

int kerchunk_hid_read_cor(void)
{
    if (g_fd < 0)
        return -1;

    unsigned char buf[4] = {0};
    ssize_t n = read(g_fd, buf, sizeof(buf));
    if (n < 1)
        return -1;  /* No data available (non-blocking) */

    int bit = (buf[0] >> g_cor_bit) & 1;
    return g_cor_active_low ? !bit : bit;
}

int kerchunk_hid_set_ptt(int active)
{
    if (g_fd < 0)
        return -1;

    unsigned char buf[4] = {0};

    /* Read current state */
    /* CM119: output report byte sets GPIO bits */
    if (active)
        buf[0] = (unsigned char)(1 << g_ptt_bit);
    else
        buf[0] = 0;

    ssize_t n = write(g_fd, buf, sizeof(buf));
    if (n < 0) {
        KERCHUNK_LOG_E(LOG_MOD, "ptt write failed");
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
