/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * main.c — nautilusctl, the command line.
 *
 * The nautilusctld service is what talks to the LCD; this program only
 * sends it commands over the Unix socket (ipc.c). The exception is
 * `convert`, which runs locally and does not use the LCD.
 */
#include "image.h"
#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    printf("Usage: %s <command> [argument]\n\n"
           "  status                 service and LCD state\n"
           "  show <file>            display a JPG, PNG or animated GIF\n"
           "  brightness <0-100>     backlight brightness\n"
           "  rotation <0-3>         0=0°, 1=90°, 2=180°, 3=270°\n"
           "  cpu [key=value ...]    CPU temperature screen; keys: font, fg, bg, label, image\n"
           "                         image=<file or library name> sets a background (image= clears it)\n"
           "                         e.g. cpu font=\"JetBrains Mono Bold\" fg=#5aa9ff label=CPU\n"
           "  stop                   stop sending images\n"
           "  hardware               return to the Corsair default screen\n"
           "  info                   firmware version\n"
           "  convert <file> <dir>   write the 480x480 JPEG frames (no LCD needed)\n\n"
           "Commands are carried out by the nautilusctld service\n"
           "(systemctl --user status nautilusctld).\n",
           prog);
}

/* Send a command to the service and print the reply. */
static int request(const char *command)
{
    GError *err = NULL;
    char *reply = ipc_request(command, &err);
    if (!reply) {
        fprintf(stderr, "error: the nautilusctld service did not respond (%s)\n"
                        "       start it with: systemctl --user start nautilusctld\n",
                err->message);
        g_error_free(err);
        return 1;
    }
    int ok = g_str_has_prefix(reply, "OK");
    const char *text = reply + (ok ? 2 : 3);
    while (*text == ' ')
        text++;
    fprintf(ok ? stdout : stderr, "%s%s\n", ok ? "" : "error: ", text);
    g_free(reply);
    return ok ? 0 : 1;
}

static int cmd_status(void)
{
    GError *err = NULL;
    char *reply = ipc_request("STATUS", &err);
    if (!reply) {
        printf("Service    : stopped (%s)\n", err->message);
        g_error_free(err);
        return 1;
    }
    GHashTable *s = ipc_parse_status(reply);
    const char *mode = g_hash_table_lookup(s, "mode");
    printf("Service    : running\n");
    printf("LCD        : %s\n", g_strcmp0(g_hash_table_lookup(s, "connected"), "1") == 0
                              ? "connected" : "disconnected (retrying)");
    printf("Mode       : %s%s\n", mode ? mode : "?",
           g_strcmp0(g_hash_table_lookup(s, "playing"), "1") == 0 ? " (sending frames)" : "");
    if (g_strcmp0(mode, "image") == 0)
        printf("Image      : %s\n", (char *)g_hash_table_lookup(s, "image"));
    if (g_strcmp0(mode, "cpu") == 0)
        printf("CPU style  : font \"%s\", text %s, background %s, label \"%s\"\n",
               (char *)g_hash_table_lookup(s, "cpu_font"), (char *)g_hash_table_lookup(s, "cpu_fg"),
               (char *)g_hash_table_lookup(s, "cpu_bg"), (char *)g_hash_table_lookup(s, "cpu_label"));
    if (g_strcmp0(mode, "cpu") == 0) {
        const char *bgi = g_hash_table_lookup(s, "cpu_image");
        printf("Background : %s\n", bgi && bgi[0] ? bgi : "(solid color)");
    }
    printf("CPU temp   : %s°C\n", (char *)g_hash_table_lookup(s, "temp"));
    printf("Brightness : %s%%\n", (char *)g_hash_table_lookup(s, "brightness"));
    printf("Rotation   : %s\n", (char *)g_hash_table_lookup(s, "rotation"));
    g_hash_table_unref(s);
    g_free(reply);
    return 0;
}

static int cmd_convert(const char *in, const char *outdir)
{
    animation anim;
    if (image_load(in, &anim) != 0) {
        fprintf(stderr, "error: %s\n", image_error());
        return 1;
    }
    int rc = 0;
    for (int i = 0; i < anim.count && rc == 0; i++) {
        char *path = g_strdup_printf("%s/frame_%03d.jpg", outdir, i);
        GError *e = NULL;
        if (!g_file_set_contents(path, (const char *)anim.frames[i].jpeg,
                                 (gssize)anim.frames[i].size, &e)) {
            fprintf(stderr, "error: %s\n", e->message);
            g_error_free(e);
            rc = 1;
        } else {
            printf("%s  (%zu bytes, %d ms)\n", path, anim.frames[i].size, anim.frames[i].delay_ms);
        }
        g_free(path);
    }
    animation_free(&anim);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return argc < 2 ? 1 : 0;
    }
    const char *cmd = argv[1];
    const char *arg = argc > 2 ? argv[2] : NULL;

    if (strcmp(cmd, "status") == 0)
        return cmd_status();
    if (strcmp(cmd, "stop") == 0)
        return request("STOP");
    if (strcmp(cmd, "hardware") == 0)
        return request("HARDWARE");
    if (strcmp(cmd, "info") == 0)
        return request("INFO");
    if (strcmp(cmd, "cpu") == 0) {
        /* Join the key=value pairs with TAB (values may contain spaces). */
        GString *line = g_string_new("CPU ");
        for (int i = 2; i < argc; i++) {
            if (!strchr(argv[i], '=')) {
                fprintf(stderr, "error: expected key=value, got '%s'\n", argv[i]);
                g_string_free(line, TRUE);
                return 1;
            }
            if (i > 2)
                g_string_append_c(line, '\t');
            /* image=file: if it exists here, send the absolute path. */
            char *abs = NULL;
            if (g_str_has_prefix(argv[i], "image=") && argv[i][6] && (abs = realpath(argv[i] + 6, NULL))) {
                g_string_append_printf(line, "image=%s", abs);
                free(abs);
            } else {
                g_string_append(line, argv[i]);
            }
        }
        int rc = request(line->str);
        g_string_free(line, TRUE);
        return rc;
    }
    if (strcmp(cmd, "convert") == 0) {
        if (argc != 4) {
            fprintf(stderr, "usage: %s convert <file> <dir>\n", argv[0]);
            return 1;
        }
        return cmd_convert(argv[2], argv[3]);
    }

    if (!arg || argc > 3) {
        fprintf(stderr, "error: '%s' takes exactly one argument (see --help)\n", cmd);
        return 1;
    }

    const char *verb = NULL;
    char *value = NULL;
    if (strcmp(cmd, "show") == 0) {
        /* The service runs in another directory: send the absolute path. */
        value = realpath(arg, NULL);
        if (!value) {
            fprintf(stderr, "error: file not found: %s\n", arg);
            return 1;
        }
        verb = "SHOW";
    } else if (strcmp(cmd, "brightness") == 0) {
        verb = "BRIGHTNESS";
    } else if (strcmp(cmd, "rotation") == 0) {
        verb = "ROTATION";
    } else {
        fprintf(stderr, "error: unknown command '%s' (see --help)\n", cmd);
        return 1;
    }

    char *line = g_strdup_printf("%s %s", verb, value ? value : arg);
    int rc = request(line);
    g_free(line);
    free(value);   /* allocated by realpath() */
    return rc;
}
