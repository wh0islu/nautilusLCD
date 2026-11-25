/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * hid.c — /dev/hidrawN access implementation.
 */
#include "hid.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int hid_find(uint16_t vid, const uint16_t *pids, size_t npids,
             char *path, size_t pathlen)
{
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir)
        return -1;

    struct dirent *e;
    int found = -1;

    while (found != 0 && (e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "hidraw", 6) != 0)
            continue;

        /*
         * The HID device's uevent file has a line like:
         *   HID_ID=0003:00001B1C:00000C57
         * where 0003 = USB bus, followed by vendor and product.
         */
        char uevent[300];
        snprintf(uevent, sizeof uevent, "/sys/class/hidraw/%s/device/uevent", e->d_name);
        FILE *f = fopen(uevent, "r");
        if (!f)
            continue;

        char line[256];
        while (fgets(line, sizeof line, f)) {
            unsigned bus, v, p;
            if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &v, &p) != 3)
                continue;
            for (size_t i = 0; i < npids; i++) {
                if (bus == 0x03 && v == vid && p == pids[i]) {
                    snprintf(path, pathlen, "/dev/%s", e->d_name);
                    found = 0;
                }
            }
        }
        fclose(f);
    }

    closedir(dir);
    return found;
}

int hid_open(hid_device *dev, const char *path)
{
    snprintf(dev->path, sizeof dev->path, "%s", path);
    /* O_CLOEXEC: don't leak the descriptor into child processes. */
    dev->fd = open(path, O_RDWR | O_CLOEXEC);
    return dev->fd < 0 ? -1 : 0;
}

void hid_close(hid_device *dev)
{
    if (dev->fd >= 0)
        close(dev->fd);
    dev->fd = -1;
}

int hid_write(hid_device *dev, const uint8_t *buf, size_t len)
{
    /* With hidraw, one write() = one whole report; partial writes are not useful. */
    ssize_t n = write(dev->fd, buf, len);
    if (n < 0)
        return -1;
    if ((size_t)n != len) {
        errno = EIO;
        return -1;
    }
    return 0;
}

int hid_set_feature(hid_device *dev, const uint8_t *buf, size_t len)
{
    /*
     * HIDIOCSFEATURE(len) is a macro that encodes the buffer size in the
     * ioctl number itself. The kernel issues a SET_REPORT over the USB
     * control endpoint.
     */
    return ioctl(dev->fd, HIDIOCSFEATURE(len), buf) < 0 ? -1 : 0;
}

int hid_get_feature(hid_device *dev, uint8_t *buf, size_t len)
{
    return ioctl(dev->fd, HIDIOCGFEATURE(len), buf);
}
