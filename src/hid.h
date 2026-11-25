/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * hid.h — direct access to a USB HID device through /dev/hidrawN.
 *
 * The Linux kernel exposes each HID interface as a character device
 * (/dev/hidraw0, /dev/hidraw1, ...). Through it we talk to the device
 * using plain system calls:
 *
 *   write(fd, buf, n)              -> sends an "output report"
 *   ioctl(fd, HIDIOCSFEATURE(n))   -> sends a "feature report"
 *   ioctl(fd, HIDIOCGFEATURE(n))   -> reads a "feature report"
 *
 * In every case, the first byte of the buffer is the Report ID.
 */
#ifndef HID_H
#define HID_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int  fd;
    char path[64];          /* e.g. "/dev/hidraw1" */
} hid_device;

/*
 * Search /sys/class/hidraw for a device with the given vendor and one of
 * the given product IDs. Needs no special permission, since it only
 * reads sysfs. Returns 0 and fills `path` when found.
 */
int hid_find(uint16_t vid, const uint16_t *pids, size_t npids,
             char *path, size_t pathlen);

int  hid_open(hid_device *dev, const char *path);
void hid_close(hid_device *dev);

/* Return 0 on success, -1 on error (errno set). */
int hid_write(hid_device *dev, const uint8_t *buf, size_t len);
int hid_set_feature(hid_device *dev, const uint8_t *buf, size_t len);

/* Returns the number of bytes read, or -1. buf[0] must hold the Report ID. */
int hid_get_feature(hid_device *dev, uint8_t *buf, size_t len);

#endif
