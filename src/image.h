/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * image.h — turns image files into frames ready for the LCD.
 *
 * Pipeline:
 *   file (JPG/PNG/GIF) --decode--> RGB pixels (any size)
 *     --resize/crop--> 480x480 RGB
 *     --encode--> JPEG (the format the LCD firmware understands)
 *
 * A GIF becomes several frames, each with its own display time.
 */
#ifndef IMAGE_H
#define IMAGE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *jpeg;      /* 480x480 JPEG (allocated by libturbojpeg) */
    size_t   size;
    int      delay_ms;  /* display time; 0 = static image */
} frame;

typedef struct {
    frame *frames;
    int    count;
} animation;

/* Load a file (JPG, PNG or GIF). Returns 0 on success. */
int image_load(const char *path, animation *out);

/* Like image_load, but only the first frame (thumbnails). */
int image_load_first(const char *path, animation *out);

/* Convert RGB pixels (3 bytes/pixel) into one 480x480 JPEG frame. */
int image_from_rgb(const uint8_t *rgb, int w, int h, animation *out);

/* Append a frame (RGB pixels of any size) to an animation. */
int image_append_rgb(animation *a, const uint8_t *rgb, int w, int h, int delay_ms);

/* Decode a 480x480 JPEG frame to RGB (buffer of 480*480*3 bytes). */
int image_decode_frame(const frame *f, uint8_t *rgb);

void animation_free(animation *a);

/* Message of the last error. */
const char *image_error(void);

#endif
