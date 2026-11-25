/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * player.c — thread that feeds the LCD continuously.
 *
 * Synchronization: a single mutex (P.lock) protects ALL state, including
 * the device. The thread only releases it while waiting
 * (g_cond_wait_until releases and re-acquires it automatically), so a
 * command coming from the socket never writes to hidraw mid-frame.
 *
 * `gen` (generation) changes on every play/stop: the thread compares the
 * generation before and after waiting, so it switches images immediately
 * instead of finishing the previous frame's delay.
 */
#include "player.h"
#include "nautilus.h"

#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#define STATIC_INTERVAL_MS 1000   /* static image resend interval */
#define RECONNECT_MS 2000         /* interval between reopen attempts */

static struct {
    GMutex lock;
    GCond cond;
    GThread *thread;
    gboolean quit;

    nautilus dev;
    gboolean dev_ok;

    animation anim;
    int idx;
    guint gen;

    int brightness, rotation;
} P;

/* Open the LCD if it is not open yet. Call with P.lock held. */
static gboolean ensure_open(void)
{
    if (P.dev_ok)
        return TRUE;
    if (nautilus_open(&P.dev) != NAUTILUS_OK)
        return FALSE;
    P.dev_ok = TRUE;
    /* Reconnected: the firmware may have forgotten the settings. */
    nautilus_set_brightness(&P.dev, P.brightness);
    nautilus_set_rotation(&P.dev, P.rotation);
    g_message("LCD connected on %s", nautilus_path(&P.dev));
    return TRUE;
}

static void drop_device(const char *why)
{
    if (P.dev_ok) {
        g_warning("LCD disconnected (%s); retrying", why);
        nautilus_close(&P.dev);
    }
    P.dev_ok = FALSE;
}

/* Wait up to `ms` or until someone signals a change. Call with P.lock held. */
static void wait_ms(int ms, guint gen)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * G_TIME_SPAN_MILLISECOND;
    while (!P.quit && P.gen == gen && g_cond_wait_until(&P.cond, &P.lock, end))
        ;
}

static gpointer thread_main(gpointer unused)
{
    (void)unused;
    g_mutex_lock(&P.lock);
    while (!P.quit) {
        if (P.anim.count == 0) {               /* nothing to show: sleep */
            g_cond_wait(&P.cond, &P.lock);
            continue;
        }
        guint gen = P.gen;
        if (!ensure_open()) {
            wait_ms(RECONNECT_MS, gen);
            continue;
        }

        const frame *f = &P.anim.frames[P.idx];
        if (nautilus_send_frame(&P.dev, f->jpeg, f->size) != 0) {
            drop_device(g_strerror(errno));
            continue;
        }
        int ms = f->delay_ms > 0 ? f->delay_ms : STATIC_INTERVAL_MS;
        P.idx = (P.idx + 1) % P.anim.count;
        wait_ms(ms, gen);
    }
    g_mutex_unlock(&P.lock);
    return NULL;
}

void player_init(int brightness, int rotation)
{
    g_mutex_init(&P.lock);
    g_cond_init(&P.cond);
    P.dev.hid.fd = -1;
    P.brightness = CLAMP(brightness, 0, 100);
    P.rotation = CLAMP(rotation, 0, 3);
    P.thread = g_thread_new("lcd-player", thread_main, NULL);

    /* Try to apply the settings right away, even with nothing to show. */
    g_mutex_lock(&P.lock);
    ensure_open();
    g_mutex_unlock(&P.lock);
}

void player_shutdown(void)
{
    g_mutex_lock(&P.lock);
    P.quit = TRUE;
    g_cond_broadcast(&P.cond);
    g_mutex_unlock(&P.lock);
    g_thread_join(P.thread);

    animation_free(&P.anim);
    if (P.dev_ok)
        nautilus_close(&P.dev);
}

void player_play(animation *anim)
{
    g_mutex_lock(&P.lock);
    animation_free(&P.anim);
    P.anim = *anim;
    anim->frames = NULL;
    anim->count = 0;
    P.idx = 0;
    P.gen++;
    g_cond_broadcast(&P.cond);
    g_mutex_unlock(&P.lock);
}

void player_stop(void)
{
    g_mutex_lock(&P.lock);
    animation_free(&P.anim);
    P.idx = 0;
    P.gen++;
    g_cond_broadcast(&P.cond);
    g_mutex_unlock(&P.lock);
}

/* Apply a setting (brightness/rotation) if the LCD is there; otherwise just store it. */
static int apply_setting(int *slot, int value, int (*fn)(nautilus *, int))
{
    g_mutex_lock(&P.lock);
    *slot = value;
    int rc = 1;
    if (ensure_open()) {
        rc = fn(&P.dev, value) == 0 ? 0 : -1;
        if (rc != 0)
            drop_device(g_strerror(errno));
    }
    g_mutex_unlock(&P.lock);
    return rc;
}

int player_set_brightness(int percent)
{
    if (percent < 0 || percent > 100)
        return -1;
    return apply_setting(&P.brightness, percent, nautilus_set_brightness);
}

int player_set_rotation(int rotation)
{
    if (rotation < 0 || rotation > 3)
        return -1;
    return apply_setting(&P.rotation, rotation, nautilus_set_rotation);
}

int player_hardware(void)
{
    player_stop();
    g_mutex_lock(&P.lock);
    int rc = -1;
    if (ensure_open()) {
        rc = nautilus_hardware_mode(&P.dev);
        if (rc != 0)
            drop_device(g_strerror(errno));
    }
    g_mutex_unlock(&P.lock);
    return rc;
}

int player_firmware(char *out, size_t len)
{
    uint8_t raw[NAUTILUS_FEATURE_SIZE];
    g_mutex_lock(&P.lock);
    int rc = -1;
    if (ensure_open() && nautilus_read_firmware(&P.dev, raw) == 0) {
        /* Bytes 6..12 hold the version as ASCII text ("0.3.0.5"). */
        snprintf(out, len, "%c.%c.%c.%c", raw[6], raw[8], raw[10], raw[12]);
        rc = 0;
    }
    g_mutex_unlock(&P.lock);
    return rc;
}

bool player_connected(void)
{
    g_mutex_lock(&P.lock);
    bool ok = P.dev_ok;
    g_mutex_unlock(&P.lock);
    return ok;
}

bool player_playing(void)
{
    g_mutex_lock(&P.lock);
    bool playing = P.anim.count > 0;
    g_mutex_unlock(&P.lock);
    return playing;
}
