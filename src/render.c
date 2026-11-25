/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * render.c — temperature screen drawn with Cairo (2D graphics) and
 * Pango (text layout, font selection through fontconfig).
 *
 * Per-frame flow:
 *   background (solid color OR decoded background-image frame)
 *   -> [with image] radial darkening in the center, for legibility
 *   -> label + number (with a shadow when there is an image)
 *   -> Cairo pixels -> RGB -> image_append_rgb() -> JPEG
 */
#include "render.h"
#include "nautilus.h"
#include "sensors.h"

#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W NAUTILUS_LCD_W
#define H NAUTILUS_LCD_H

void cpu_style_default(cpu_style *st)
{
    memset(st, 0, sizeof *st);
    snprintf(st->font, sizeof st->font, "Sans Bold");
    snprintf(st->fg, sizeof st->fg, "#ffffff");
    snprintf(st->bg, sizeof st->bg, "#000000");
    snprintf(st->label, sizeof st->label, "CPU");
}

int parse_hex_color(const char *s, double rgb[3])
{
    if (!s || s[0] != '#' || strlen(s) != 7)
        return -1;
    for (int i = 0; i < 3; i++) {
        char hex[3] = { s[1 + i * 2], s[2 + i * 2], 0 };
        char *end;
        unsigned long x = strtoul(hex, &end, 16);
        if (*end)
            return -1;
        rgb[i] = x / 255.0;
    }
    return 0;
}

/*
 * Draw `text` horizontally centered, with its VISUAL center (ink rect:
 * only the inked area) at `cy`. With `shadow`, first draw an offset dark
 * copy to make the text stand out over photos.
 */
static void draw_centered(cairo_t *cr, PangoLayout *layout, const char *text, double cy,
                          const double rgb[3], double alpha, int shadow)
{
    PangoRectangle ink;
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
    double x = (W - ink.width) / 2.0 - ink.x;
    double y = cy - ink.height / 2.0 - ink.y;

    if (shadow) {
        cairo_set_source_rgba(cr, 0, 0, 0, 0.55 * alpha);
        cairo_move_to(cr, x + 3, y + 4);
        pango_cairo_show_layout(cr, layout);
    }
    cairo_set_source_rgba(cr, rgb[0], rgb[1], rgb[2], alpha);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
}

/* Text (label + number) on top of whatever is already on the surface. */
static void draw_overlay(cairo_t *cr, const cpu_style *st, int temp_c, int over_image)
{
    double fg[3] = { 1, 1, 1 };
    parse_hex_color(st->fg, fg);

    if (over_image) {
        /* Darken the center (where the text is) and keep the edges. */
        cairo_pattern_t *g = cairo_pattern_create_radial(W / 2.0, H / 2.0, 0, W / 2.0, H / 2.0, W / 2.0);
        cairo_pattern_add_color_stop_rgba(g, 0.0, 0, 0, 0, 0.55);
        cairo_pattern_add_color_stop_rgba(g, 0.6, 0, 0, 0, 0.35);
        cairo_pattern_add_color_stop_rgba(g, 1.0, 0, 0, 0, 0.0);
        cairo_set_source(cr, g);
        cairo_paint(cr);
        cairo_pattern_destroy(g);
    }

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *fd = pango_font_description_from_string(st->font[0] ? st->font : "Sans Bold");
    int has_label = st->label[0] != '\0';

    char num[16];
    if (temp_c == CPU_TEMP_UNAVAILABLE)
        snprintf(num, sizeof num, "--");
    else
        snprintf(num, sizeof num, "%d°", temp_c);
    pango_font_description_set_absolute_size(fd, 170 * PANGO_SCALE);
    pango_layout_set_font_description(layout, fd);
    draw_centered(cr, layout, num, has_label ? H / 2.0 + 30 : H / 2.0, fg, 1.0, over_image);

    if (has_label) {
        pango_font_description_set_absolute_size(fd, 44 * PANGO_SCALE);
        pango_layout_set_font_description(layout, fd);
        draw_centered(cr, layout, st->label, H / 2.0 - 105, fg, over_image ? 0.85 : 0.6, over_image);
    }
    pango_font_description_free(fd);
    g_object_unref(layout);
}

/*
 * CAIRO_FORMAT_RGB24 stores each pixel in a native 32-bit word (0xXXRRGGBB).
 * We access it as uint32_t so we don't depend on byte order.
 */
static void rgb_to_surface(const uint8_t *rgb, cairo_surface_t *surf)
{
    cairo_surface_flush(surf);
    uint8_t *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)(data + (size_t)y * stride);
        for (int x = 0; x < W; x++) {
            const uint8_t *p = rgb + ((size_t)y * W + x) * 3;
            row[x] = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    cairo_surface_mark_dirty(surf);
}

static void surface_to_rgb(cairo_surface_t *surf, uint8_t *rgb)
{
    cairo_surface_flush(surf);
    const uint8_t *data = cairo_image_surface_get_data(surf);
    int stride = cairo_image_surface_get_stride(surf);
    for (int y = 0; y < H; y++) {
        const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);
        for (int x = 0; x < W; x++) {
            uint8_t *p = rgb + ((size_t)y * W + x) * 3;
            p[0] = (row[x] >> 16) & 0xff;
            p[1] = (row[x] >> 8) & 0xff;
            p[2] = row[x] & 0xff;
        }
    }
}

int render_cpu(const cpu_style *st, int temp_c, const animation *background, animation *out)
{
    out->frames = NULL;
    out->count = 0;

    int has_bg = background && background->count > 0;
    int n = has_bg ? background->count : 1;
    uint8_t *rgb = malloc((size_t)W * H * 3);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
    int rc = rgb ? 0 : -1;

    for (int i = 0; i < n && rc == 0; i++) {
        if (has_bg) {
            /* Background frames are already 480x480 (same pipeline as images). */
            if (image_decode_frame(&background->frames[i], rgb) != 0) {
                rc = -1;
                break;
            }
            rgb_to_surface(rgb, surf);
        }
        cairo_t *cr = cairo_create(surf);
        if (!has_bg) {
            double bg[3] = { 0, 0, 0 };
            parse_hex_color(st->bg, bg);
            cairo_set_source_rgb(cr, bg[0], bg[1], bg[2]);
            cairo_paint(cr);
        }
        draw_overlay(cr, st, temp_c, has_bg);
        cairo_destroy(cr);

        surface_to_rgb(surf, rgb);
        int delay = has_bg ? background->frames[i].delay_ms : 0;
        rc = image_append_rgb(out, rgb, W, H, delay);
    }

    cairo_surface_destroy(surf);
    free(rgb);
    if (rc != 0)
        animation_free(out);
    return rc;
}
