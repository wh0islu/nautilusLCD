/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * render.h — draws the CPU temperature screen (Cairo + Pango).
 *
 * Used by the service (for the LCD) and by the window (for the preview),
 * so the preview is drawn by the same code that produces the real screen.
 */
#ifndef RENDER_H
#define RENDER_H

#include "image.h"

typedef struct {
    char font[128];      /* Pango description without size: "Inter Bold", "JetBrains Mono" */
    char fg[8];          /* "#rrggbb" — text color */
    char bg[8];          /* "#rrggbb" — background color (when there is no image) */
    char label[32];      /* optional text above the temperature ("CPU") */
    char bg_image[512];  /* absolute path of a background image/GIF, or "" */
} cpu_style;

void cpu_style_default(cpu_style *st);

/*
 * Build the screen with `temp_c` (or "--" if unavailable).
 *   background == NULL or empty -> 1 frame with a solid color background;
 *   background with N frames    -> N frames (same timings), the text
 *                                  drawn on top of each one.
 * `background` must come from image_load() (480x480 JPEG frames).
 */
int render_cpu(const cpu_style *st, int temp_c, const animation *background, animation *out);

/* Validate "#rrggbb" and convert to 0..1. */
int parse_hex_color(const char *s, double rgb[3]);

#endif
