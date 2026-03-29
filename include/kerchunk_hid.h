/*
 * kerchunk_hid.h — HID driver for RIM-Lite COR/PTT
 */

#ifndef KERCHUNK_HID_H
#define KERCHUNK_HID_H

/* HID configuration */
typedef struct {
    const char *device;         /* e.g., /dev/rimlite (udev symlink) */
    int cor_bit;                /* Bit number for COR signal */
    int cor_polarity;           /* 0 = active_high, 1 = active_low */
    int ptt_bit;                /* Bit number for PTT output */
} kerchunk_hid_config_t;

/* Initialize HID interface. Returns 0 on success. */
int  kerchunk_hid_init(const kerchunk_hid_config_t *cfg);

/* Shutdown HID interface. */
void kerchunk_hid_shutdown(void);

/* Read COR state. Returns 1 if carrier detected, 0 if not, -1 on error. */
int  kerchunk_hid_read_cor(void);

/* Set PTT state. active=1 to key up, active=0 to unkey. Returns 0 on success. */
int  kerchunk_hid_set_ptt(int active);

/* Check if HID device is available. */
int  kerchunk_hid_available(void);

#endif /* KERCHUNK_HID_H */
