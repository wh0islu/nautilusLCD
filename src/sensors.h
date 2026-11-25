/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * sensors.h — CPU temperature via hwmon (/sys/class/hwmon).
 *
 * The kernel exposes sensors in /sys/class/hwmon/hwmonN/:
 *   name           -> driver ("coretemp" Intel, "k10temp" AMD)
 *   tempX_label    -> what the sensor measures ("Package id 0", "Tctl"...)
 *   tempX_input    -> value in thousandths of a degree Celsius
 * N changes between boots, so we search by driver name.
 */
#ifndef SENSORS_H
#define SENSORS_H

#define CPU_TEMP_UNAVAILABLE (-1000)

/* CPU package temperature in °C, or CPU_TEMP_UNAVAILABLE. */
int cpu_temp_read(void);

#endif
