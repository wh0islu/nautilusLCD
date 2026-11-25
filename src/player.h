/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * player.h — owner of the LCD inside the service (nautilusctld).
 *
 * One long-lived thread:
 *   - keeps /dev/hidraw open, reopening it if it drops (USB reset,
 *     suspend, cable) and re-applying brightness/rotation on reconnect;
 *   - sends the current animation's frames in a loop, because the LCD
 *     firmware falls back to its default screen when frames stop coming.
 *
 * Every function is safe to call from any thread.
 */
#ifndef PLAYER_H
#define PLAYER_H

#include "image.h"

#include <stdbool.h>
#include <stddef.h>

void player_init(int brightness, int rotation);
void player_shutdown(void);

/* Start showing `anim` (takes ownership; `anim` is left empty). */
void player_play(animation *anim);
void player_stop(void);

/* 0 = applied; 1 = stored, will apply on reconnect; -1 = error. */
int player_set_brightness(int percent);
int player_set_rotation(int rotation);

/* Stop sending and return the screen to the firmware. 0 = ok, -1 = error/disconnected. */
int player_hardware(void);

/* Firmware version ("0.3.0.5"). 0 = ok. */
int player_firmware(char *out, size_t len);

bool player_connected(void);
bool player_playing(void);

#endif
