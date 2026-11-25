/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * image.c — decoding (libjpeg-turbo, libpng, giflib),
 * resizing (own implementation, bilinear) and
 * JPEG encoding (libjpeg-turbo).
 */
#include "image.h"
#include "nautilus.h"

#include <gif_lib.h>
#include <png.h>
#include <turbojpeg.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_W NAUTILUS_LCD_W
#define OUT_H NAUTILUS_LCD_H
#define JPEG_QUALITY 90
#define MAX_GIF_FRAMES 1000
#define MAX_INPUT_BYTES (64L * 1024 * 1024)

static char err_msg[512];

const char *image_error(void)
{
    return err_msg;
}

static int fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_msg, sizeof err_msg, fmt, ap);
    va_end(ap);
    return -1;
}

/* Simple RGB image: 3 bytes per pixel, row after row. */
typedef struct {
    uint8_t *px;
    int w, h;
} rgb_image;

/* ------------------------------------------------------------------ */
/* Resizing: "cover" = fill 480x480 and crop the excess.              */
/* ------------------------------------------------------------------ */

/*
 * For each output pixel we compute the matching position in the source
 * image and do bilinear interpolation with its 4 neighbours. The scale
 * is the LARGER of the two ratios so the image covers the whole screen;
 * the (centered) excess is cropped, since the screen is round anyway.
 */
static int fit_cover(const rgb_image *src, uint8_t *dst)
{
    double scale = fmax((double)OUT_W / src->w, (double)OUT_H / src->h);
    double off_x = (src->w - OUT_W / scale) / 2.0;   /* left crop */
    double off_y = (src->h - OUT_H / scale) / 2.0;   /* top crop */

    for (int y = 0; y < OUT_H; y++) {
        /* +0.5 / -0.5: align pixel centers, not corners. */
        double sy = (y + 0.5) / scale + off_y - 0.5;
        int y0 = (int)floor(sy);
        double fy = sy - y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 < 0) y1 = 0;
        if (y0 >= src->h) y0 = src->h - 1;
        if (y1 >= src->h) y1 = src->h - 1;

        for (int x = 0; x < OUT_W; x++) {
            double sx = (x + 0.5) / scale + off_x - 0.5;
            int x0 = (int)floor(sx);
            double fx = sx - x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 < 0) x1 = 0;
            if (x0 >= src->w) x0 = src->w - 1;
            if (x1 >= src->w) x1 = src->w - 1;

            const uint8_t *p00 = src->px + ((size_t)y0 * src->w + x0) * 3;
            const uint8_t *p01 = src->px + ((size_t)y0 * src->w + x1) * 3;
            const uint8_t *p10 = src->px + ((size_t)y1 * src->w + x0) * 3;
            const uint8_t *p11 = src->px + ((size_t)y1 * src->w + x1) * 3;
            uint8_t *out = dst + ((size_t)y * OUT_W + x) * 3;

            for (int c = 0; c < 3; c++) {
                double top = p00[c] + (p01[c] - p00[c]) * fx;
                double bot = p10[c] + (p11[c] - p10[c]) * fx;
                out[c] = (uint8_t)lround(top + (bot - top) * fy);
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* JPEG encoding                                                       */
/* ------------------------------------------------------------------ */

/*
 * Encode 480x480 RGB as JPEG. If the result does not fit in the 256
 * chunks the protocol allows (~254 KB), lower the quality.
 */
static int encode_jpeg(const uint8_t *rgb, frame *f)
{
    tjhandle tj = tj3Init(TJINIT_COMPRESS);
    if (!tj)
        return fail("tj3Init failed");

    int rc = -1;
    tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_420);

    for (int q = JPEG_QUALITY; q >= 30; q -= 10) {
        unsigned char *buf = NULL;
        size_t size = 0;
        tj3Set(tj, TJPARAM_QUALITY, q);
        if (tj3Compress8(tj, rgb, OUT_W, 0, OUT_H, TJPF_RGB, &buf, &size) != 0) {
            fail("failed to encode JPEG: %s", tj3GetErrorStr(tj));
            break;
        }
        if (size <= NAUTILUS_MAX_FRAME) {
            f->jpeg = buf;
            f->size = size;
            rc = 0;
            break;
        }
        tj3Free(buf);
    }
    if (rc != 0 && !err_msg[0])
        fail("image does not fit the protocol limit even at low quality");

    tj3Destroy(tj);
    return rc;
}

/* Append a frame (RGB of any size) to the animation. */
static int push_frame(animation *a, const rgb_image *img, int delay_ms)
{
    uint8_t *scaled = malloc((size_t)OUT_W * OUT_H * 3);
    if (!scaled)
        return fail("out of memory");

    frame f = { NULL, 0, delay_ms };
    fit_cover(img, scaled);
    int rc = encode_jpeg(scaled, &f);
    free(scaled);
    if (rc != 0)
        return -1;

    frame *grown = realloc(a->frames, sizeof *grown * (a->count + 1));
    if (!grown) {
        tj3Free(f.jpeg);
        return fail("out of memory");
    }
    a->frames = grown;
    a->frames[a->count++] = f;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Decoders                                                            */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail("cannot open %s", path);
        return NULL;
    }
    uint8_t *data = NULL;
    long len = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        len = ftell(f);
    if (len <= 0 || len > MAX_INPUT_BYTES) {
        fail("invalid file size (max 64 MB)");
    } else if (fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)len))) {
        if (fread(data, 1, (size_t)len, f) != (size_t)len) {
            free(data);
            data = NULL;
            fail("error reading %s", path);
        }
    }
    fclose(f);
    *size = (size_t)len;
    return data;
}

static int load_jpeg(const uint8_t *data, size_t size, animation *a)
{
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj)
        return fail("tj3Init failed");

    int rc = -1;
    rgb_image img = { NULL, 0, 0 };

    if (tj3DecompressHeader(tj, data, size) != 0) {
        fail("invalid JPEG: %s", tj3GetErrorStr(tj));
        goto out;
    }
    img.w = tj3Get(tj, TJPARAM_JPEGWIDTH);
    img.h = tj3Get(tj, TJPARAM_JPEGHEIGHT);
    img.px = malloc((size_t)img.w * img.h * 3);
    if (!img.px) {
        fail("out of memory");
        goto out;
    }
    if (tj3Decompress8(tj, data, size, img.px, 0, TJPF_RGB) != 0) {
        fail("failed to decode JPEG: %s", tj3GetErrorStr(tj));
        goto out;
    }
    rc = push_frame(a, &img, 0);
out:
    free(img.px);
    tj3Destroy(tj);
    return rc;
}

static int load_png(const uint8_t *data, size_t size, animation *a)
{
    /* libpng "simplified" API: converts any PNG to RGB. */
    png_image png;
    memset(&png, 0, sizeof png);
    png.version = PNG_IMAGE_VERSION;

    if (!png_image_begin_read_from_memory(&png, data, size))
        return fail("invalid PNG: %s", png.message);

    png.format = PNG_FORMAT_RGB;
    rgb_image img = { malloc(PNG_IMAGE_SIZE(png)), (int)png.width, (int)png.height };
    if (!img.px) {
        png_image_free(&png);
        return fail("out of memory");
    }

    /* Transparent areas are blended onto a black background. */
    png_color black = { 0, 0, 0 };
    if (!png_image_finish_read(&png, &black, img.px, 0, NULL)) {
        free(img.px);
        return fail("failed to decode PNG: %s", png.message);
    }

    int rc = push_frame(a, &img, 0);
    free(img.px);
    return rc;
}

/*
 * Animated GIF: each image stored in the file may cover only a rectangle
 * of the screen and have transparent pixels. So we keep an RGBA "virtual
 * screen" (canvas), draw each frame on top of it and apply the GIF's
 * disposal method before the next frame.
 */
static int load_gif(const char *path, animation *a, int max_frames)
{
    int err = 0;
    GifFileType *gif = DGifOpenFileName(path, &err);
    if (!gif)
        return fail("invalid GIF: %s", GifErrorString(err));

    int rc = -1;
    uint8_t *canvas = NULL, *saved = NULL;
    rgb_image rgb = { NULL, gif->SWidth, gif->SHeight };

    if (DGifSlurp(gif) != GIF_OK) {      /* read all frames into memory */
        fail("failed to read GIF: %s", GifErrorString(gif->Error));
        goto out;
    }
    if (gif->SWidth <= 0 || gif->SHeight <= 0 || gif->ImageCount <= 0) {
        fail("GIF has no images");
        goto out;
    }

    size_t npx = (size_t)gif->SWidth * gif->SHeight;
    canvas = calloc(npx, 4);             /* RGBA, starts transparent */
    saved = malloc(npx * 4);
    rgb.px = malloc(npx * 3);
    if (!canvas || !saved || !rgb.px) {
        fail("out of memory");
        goto out;
    }

    int frames = gif->ImageCount < max_frames ? gif->ImageCount : max_frames;
    for (int i = 0; i < frames; i++) {
        SavedImage *si = &gif->SavedImages[i];
        GifImageDesc *d = &si->ImageDesc;
        ColorMapObject *cmap = d->ColorMap ? d->ColorMap : gif->SColorMap;

        GraphicsControlBlock gcb = { DISPOSAL_UNSPECIFIED, false, 0, NO_TRANSPARENT_COLOR };
        DGifSavedExtensionToGCB(gif, i, &gcb);   /* frames without a GCB keep the defaults */

        if (gcb.DisposalMode == DISPOSE_PREVIOUS)
            memcpy(saved, canvas, npx * 4);

        /* Draw the frame onto the canvas (giflib already deinterlaces it). */
        if (cmap) {
            for (int y = 0; y < d->Height; y++) {
                int cy = d->Top + y;
                if (cy < 0 || cy >= gif->SHeight)
                    continue;
                for (int x = 0; x < d->Width; x++) {
                    int cx = d->Left + x;
                    if (cx < 0 || cx >= gif->SWidth)
                        continue;
                    int idx = si->RasterBits[(size_t)y * d->Width + x];
                    if (idx == gcb.TransparentColor || idx >= cmap->ColorCount)
                        continue;
                    uint8_t *p = canvas + ((size_t)cy * gif->SWidth + cx) * 4;
                    p[0] = cmap->Colors[idx].Red;
                    p[1] = cmap->Colors[idx].Green;
                    p[2] = cmap->Colors[idx].Blue;
                    p[3] = 255;
                }
            }
        }

        /* RGBA -> RGB; anything still transparent becomes black. */
        for (size_t k = 0; k < npx; k++) {
            int opaque = canvas[k * 4 + 3] != 0;
            rgb.px[k * 3 + 0] = opaque ? canvas[k * 4 + 0] : 0;
            rgb.px[k * 3 + 1] = opaque ? canvas[k * 4 + 1] : 0;
            rgb.px[k * 3 + 2] = opaque ? canvas[k * 4 + 2] : 0;
        }

        /* DelayTime is in hundredths of a second; < 2 is usually treated as 100 ms. */
        int delay = gcb.DelayTime < 2 ? 100 : gcb.DelayTime * 10;
        if (push_frame(a, &rgb, frames > 1 ? delay : 0) != 0)
            goto out;

        /* Disposal: prepare the canvas for the next frame. */
        if (gcb.DisposalMode == DISPOSE_BACKGROUND) {
            for (int y = 0; y < d->Height; y++) {
                int cy = d->Top + y;
                if (cy < 0 || cy >= gif->SHeight)
                    continue;
                for (int x = 0; x < d->Width; x++) {
                    int cx = d->Left + x;
                    if (cx >= 0 && cx < gif->SWidth)
                        memset(canvas + ((size_t)cy * gif->SWidth + cx) * 4, 0, 4);
                }
            }
        } else if (gcb.DisposalMode == DISPOSE_PREVIOUS) {
            memcpy(canvas, saved, npx * 4);
        }
    }
    rc = 0;

out:
    free(canvas);
    free(saved);
    free(rgb.px);
    DGifCloseFile(gif, &err);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static int load_any(const char *path, animation *out, int max_frames)
{
    out->frames = NULL;
    out->count = 0;
    err_msg[0] = '\0';

    size_t size = 0;
    uint8_t *data = read_file(path, &size);
    if (!data)
        return -1;

    /* Detect the format from its "magic bytes", not the extension. */
    int rc;
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        rc = load_jpeg(data, size, out);
    else if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
        rc = load_png(data, size, out);
    else if (size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
        rc = load_gif(path, out, max_frames);
    else
        rc = fail("unsupported format (use JPG, PNG or GIF)");

    free(data);
    if (rc != 0)
        animation_free(out);
    return rc;
}

int image_load(const char *path, animation *out)
{
    return load_any(path, out, MAX_GIF_FRAMES);
}

int image_load_first(const char *path, animation *out)
{
    /* First frame only: used for thumbnails. */
    return load_any(path, out, 1);
}

int image_from_rgb(const uint8_t *rgb, int w, int h, animation *out)
{
    out->frames = NULL;
    out->count = 0;
    err_msg[0] = '\0';
    return image_append_rgb(out, rgb, w, h, 0);
}

int image_append_rgb(animation *a, const uint8_t *rgb, int w, int h, int delay_ms)
{
    rgb_image img = { (uint8_t *)rgb, w, h };   /* push_frame only reads the pixels */
    return push_frame(a, &img, delay_ms);
}

int image_decode_frame(const frame *f, uint8_t *rgb)
{
    tjhandle tj = tj3Init(TJINIT_DECOMPRESS);
    if (!tj)
        return fail("tj3Init failed");
    int rc = -1;
    if (tj3DecompressHeader(tj, f->jpeg, f->size) != 0 ||
        tj3Get(tj, TJPARAM_JPEGWIDTH) != OUT_W || tj3Get(tj, TJPARAM_JPEGHEIGHT) != OUT_H)
        fail("frame is not a 480x480 JPEG");
    else if (tj3Decompress8(tj, f->jpeg, f->size, rgb, 0, TJPF_RGB) != 0)
        fail("failed to decode frame: %s", tj3GetErrorStr(tj));
    else
        rc = 0;
    tj3Destroy(tj);
    return rc;
}

void animation_free(animation *a)
{
    for (int i = 0; i < a->count; i++)
        tj3Free(a->frames[i].jpeg);
    free(a->frames);
    a->frames = NULL;
    a->count = 0;
}
