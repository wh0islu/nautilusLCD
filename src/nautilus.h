/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * nautilus.h — Corsair Nautilus (LCD Cap) LCD protocol.
 *
 * Protocol sources:
 *   - HID report descriptor read from the device itself
 *     (/sys/class/hidraw/hidrawN/device/report_descriptor):
 *       Output  report 0x02 = 1023 bytes + 1 ID byte  -> screen frames
 *       Feature report 0x03..0x16 = 31 bytes + 1 ID   -> commands
 *   - byte meanings studied in the open source OpenLinkHub driver
 *     (src/devices/nautilusLcd/nautilusLcd.go, tag 0.9.5). No code was
 *     copied; only the packet format.
 */
#ifndef NAUTILUS_H
#define NAUTILUS_H

#include "hid.h"

#include <stddef.h>
#include <stdint.h>

#define NAUTILUS_VID          0x1b1c
#define NAUTILUS_LCD_W        480
#define NAUTILUS_LCD_H        480

#define NAUTILUS_PACKET_SIZE  1024                       /* ID + 1023 */
#define NAUTILUS_HEADER_SIZE  8
#define NAUTILUS_CHUNK_MAX    (NAUTILUS_PACKET_SIZE - NAUTILUS_HEADER_SIZE)  /* 1016 */
#define NAUTILUS_MAX_CHUNKS   256                        /* the index is 1 byte */
#define NAUTILUS_MAX_FRAME    ((size_t)NAUTILUS_CHUNK_MAX * NAUTILUS_MAX_CHUNKS)

#define NAUTILUS_FEATURE_SIZE 32                         /* ID + 31 */

typedef struct {
    hid_device hid;
} nautilus;

/* nautilus_open() errors, for specific messages. */
enum {
    NAUTILUS_OK = 0,
    NAUTILUS_NOT_FOUND = -1,
    NAUTILUS_OPEN_FAILED = -2,   /* errno holds the reason (e.g. EACCES) */
};

int  nautilus_open(nautilus *n);
void nautilus_close(nautilus *n);
const char *nautilus_path(const nautilus *n);

/* Send one 480x480 JPEG frame to the screen. */
int nautilus_send_frame(nautilus *n, const uint8_t *jpeg, size_t len);

/* Backlight brightness, 0..100. */
int nautilus_set_brightness(nautilus *n, int percent);

/* Rotation: 0=0°, 1=90°, 2=180°, 3=270°. */
int nautilus_set_rotation(nautilus *n, int rotation);

/* Read feature report 0x05 (firmware version). `raw` receives 32 bytes. */
int nautilus_read_firmware(nautilus *n, uint8_t raw[NAUTILUS_FEATURE_SIZE]);

/* Return the screen to "hardware mode" (the firmware's default screen). */
int nautilus_hardware_mode(nautilus *n);

#endif
