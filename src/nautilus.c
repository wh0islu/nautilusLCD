/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * nautilus.c — builds the LCD protocol packets.
 *
 * SCREEN FRAME (output report 0x02, always 1024 bytes)
 *
 *   byte 0    0x02        Report ID
 *   byte 1    0x05        command: image data
 *   byte 2    0x00
 *   byte 3    0x00/0x01   1 = last chunk; the screen only draws once it arrives
 *   byte 4    i           chunk index (0, 1, 2, ...)
 *   byte 5    0x00
 *   byte 6-7  len         chunk length, little-endian
 *   byte 8..  data        up to 1016 bytes of the JPEG file
 *
 * A ~40 KB JPEG becomes ~40 packets. The screen joins the chunks and, on
 * the last one, decodes the JPEG and shows it.
 *
 * COMMANDS (feature report 0x03, 32 bytes, zero-padded)
 *
 *   03 0b <0..100> 01   brightness
 *   03 0c <0..3>   01   rotation
 *   03 1e 01 01  +  03 1d 00 01   back to hardware mode
 */
#include "nautilus.h"

#include <errno.h>
#include <string.h>

/* Known product IDs for the Nautilus LCD Cap. */
static const uint16_t nautilus_pids[] = { 0x0c57, 0x0c55 };

int nautilus_open(nautilus *n)
{
    char path[64];
    n->hid.fd = -1;

    if (hid_find(NAUTILUS_VID, nautilus_pids,
                 sizeof nautilus_pids / sizeof *nautilus_pids,
                 path, sizeof path) != 0)
        return NAUTILUS_NOT_FOUND;

    if (hid_open(&n->hid, path) != 0)
        return NAUTILUS_OPEN_FAILED;

    return NAUTILUS_OK;
}

void nautilus_close(nautilus *n)
{
    hid_close(&n->hid);
}

const char *nautilus_path(const nautilus *n)
{
    return n->hid.path;
}

int nautilus_send_frame(nautilus *n, const uint8_t *jpeg, size_t len)
{
    if (len == 0 || len > NAUTILUS_MAX_FRAME) {
        errno = EMSGSIZE;
        return -1;
    }

    uint8_t pkt[NAUTILUS_PACKET_SIZE];
    size_t chunks = (len + NAUTILUS_CHUNK_MAX - 1) / NAUTILUS_CHUNK_MAX;

    for (size_t i = 0; i < chunks; i++) {
        size_t off = i * NAUTILUS_CHUNK_MAX;
        size_t sz = len - off < NAUTILUS_CHUNK_MAX ? len - off : NAUTILUS_CHUNK_MAX;

        /* Zero the whole packet: the space after the data must be clean. */
        memset(pkt, 0, sizeof pkt);
        pkt[0] = 0x02;
        pkt[1] = 0x05;
        pkt[3] = (i == chunks - 1) ? 0x01 : 0x00;
        pkt[4] = (uint8_t)i;
        pkt[6] = (uint8_t)(sz & 0xff);         /* little-endian: low byte first */
        pkt[7] = (uint8_t)(sz >> 8);
        memcpy(pkt + NAUTILUS_HEADER_SIZE, jpeg + off, sz);

        if (hid_write(&n->hid, pkt, sizeof pkt) != 0)
            return -1;
    }
    return 0;
}

/* Send 03 <cmd> <value> 01, zero-padded to 32 bytes. */
static int send_command(nautilus *n, uint8_t cmd, uint8_t value)
{
    uint8_t buf[NAUTILUS_FEATURE_SIZE] = { 0x03, cmd, value, 0x01 };
    return hid_set_feature(&n->hid, buf, sizeof buf);
}

int nautilus_set_brightness(nautilus *n, int percent)
{
    if (percent < 0 || percent > 100) {
        errno = EINVAL;
        return -1;
    }
    return send_command(n, 0x0b, (uint8_t)percent);
}

int nautilus_set_rotation(nautilus *n, int rotation)
{
    if (rotation < 0 || rotation > 3) {
        errno = EINVAL;
        return -1;
    }
    return send_command(n, 0x0c, (uint8_t)rotation);
}

int nautilus_read_firmware(nautilus *n, uint8_t raw[NAUTILUS_FEATURE_SIZE])
{
    memset(raw, 0, NAUTILUS_FEATURE_SIZE);
    raw[0] = 0x05;                      /* Report ID to read */
    return hid_get_feature(&n->hid, raw, NAUTILUS_FEATURE_SIZE) < 0 ? -1 : 0;
}

int nautilus_hardware_mode(nautilus *n)
{
    if (send_command(n, 0x1e, 0x01) != 0)
        return -1;
    return send_command(n, 0x1d, 0x00);
}
