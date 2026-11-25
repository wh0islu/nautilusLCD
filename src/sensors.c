/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * sensors.c — finds and reads the CPU package temperature sensor.
 */
#include "sensors.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

/* Path of the chosen tempX_input; cached so we don't scan on every read. */
static char cached[512];

static int read_line(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int ok = fgets(buf, (int)len, f) != NULL;
    fclose(f);
    if (!ok)
        return -1;
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

/*
 * Preference: the CPU driver's "whole package" label; if no known label
 * exists, that driver's temp1.
 */
static int find_sensor(void)
{
    static const struct { const char *driver, *label; } wanted[] = {
        { "coretemp", "Package id 0" },   /* Intel */
        { "k10temp",  "Tctl" },           /* AMD Zen */
        { "zenpower", "Tdie" },           /* AMD (alternative module) */
    };

    DIR *d = opendir("/sys/class/hwmon");
    if (!d)
        return -1;
    struct dirent *e;
    int found = -1;
    while (found != 0 && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char path[300], name[64];
        snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
        if (read_line(path, name, sizeof name) != 0)
            continue;

        for (size_t w = 0; w < sizeof wanted / sizeof *wanted && found != 0; w++) {
            if (strcmp(name, wanted[w].driver) != 0)
                continue;
            for (int i = 1; i <= 64; i++) {
                char lp[320], label[64];
                snprintf(lp, sizeof lp, "/sys/class/hwmon/%s/temp%d_label", e->d_name, i);
                if (read_line(lp, label, sizeof label) == 0 && strcmp(label, wanted[w].label) == 0) {
                    snprintf(cached, sizeof cached, "/sys/class/hwmon/%s/temp%d_input", e->d_name, i);
                    found = 0;
                    break;
                }
            }
            if (found != 0) {
                snprintf(cached, sizeof cached, "/sys/class/hwmon/%s/temp1_input", e->d_name);
                found = 0;
            }
        }
    }
    closedir(d);
    return found;
}

int cpu_temp_read(void)
{
    char buf[32];
    /* If the cached path is gone (module reloaded), search again. */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!cached[0] && find_sensor() != 0)
            return CPU_TEMP_UNAVAILABLE;
        if (read_line(cached, buf, sizeof buf) == 0) {
            long milli = 0;
            if (sscanf(buf, "%ld", &milli) == 1)
                return (int)((milli + 500) / 1000);   /* round */
        }
        cached[0] = '\0';
    }
    return CPU_TEMP_UNAVAILABLE;
}
