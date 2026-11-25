/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * daemon.c — nautilusctld, the background service and sole owner of the LCD.
 *
 * Runs as a systemd USER service (starts at login). Responsibilities:
 *   1. keep the LCD fed with frames (player.c);
 *   2. accept commands on the Unix socket (GThreadedSocketService: each
 *      client is served on a pool thread, and the `ctl` mutex runs
 *      commands one at a time);
 *   3. persist state in ~/.config/nautilusctl/config.ini and restore it
 *      on startup.
 */
#include "image.h"
#include "ipc.h"
#include "player.h"
#include "render.h"
#include "sensors.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static GMutex ctl;
static GKeyFile *cfg;
static char *cfg_path, *lib_dir;

/* Temperature screen: current style and last temperature drawn. */
static cpu_style cpu_st;
static int cpu_last = INT_MIN;
static gint64 cpu_last_render;       /* g_get_monotonic_time() of the last render */
static animation cpu_bg;             /* background image frames (cache) */
static char cpu_bg_path[512];        /* path currently loaded in cpu_bg */

/*
 * Re-rendering a GIF background costs ~8 ms per frame (0.7 s for 92
 * frames). With an animated background we limit it to one render every
 * 10 s; with a static image (8 ms) we re-render on every temperature change.
 */
#define CPU_MIN_INTERVAL_ANIMATED (10 * G_TIME_SPAN_SECOND)

/* ---------- configuration ---------- */

static char *cfg_str(const char *key, const char *def)
{
    char *v = g_key_file_get_string(cfg, "lcd", key, NULL);
    return v ? v : g_strdup(def);
}

static int cfg_int(const char *key, int def)
{
    GError *e = NULL;
    int v = g_key_file_get_integer(cfg, "lcd", key, &e);
    if (e) {
        g_error_free(e);
        return def;
    }
    return v;
}

static void cfg_save(void)
{
    char *dir = g_path_get_dirname(cfg_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    GError *e = NULL;
    if (!g_key_file_save_to_file(cfg, cfg_path, &e)) {
        g_warning("could not save %s: %s", cfg_path, e->message);
        g_error_free(e);
    }
}

/* Older versions stored only the library file name. */
static char *resolve_image(const char *v)
{
    return g_path_is_absolute(v) ? g_strdup(v) : g_build_filename(lib_dir, v, NULL);
}

/* ---------- commands ---------- */

static gboolean parse_int(const char *s, int lo, int hi, int *out)
{
    char *end;
    gint64 v = g_ascii_strtoll(s, &end, 10);
    if (end == s || *end || v < lo || v > hi)
        return FALSE;
    *out = (int)v;
    return TRUE;
}

/* ---------- CPU temperature screen ---------- */

static void cpu_load_style(void)
{
    cpu_style_default(&cpu_st);
    char *v;
    if ((v = g_key_file_get_string(cfg, "cpu", "font", NULL))) {
        g_strlcpy(cpu_st.font, v, sizeof cpu_st.font); g_free(v);
    }
    if ((v = g_key_file_get_string(cfg, "cpu", "fg", NULL))) {
        g_strlcpy(cpu_st.fg, v, sizeof cpu_st.fg); g_free(v);
    }
    if ((v = g_key_file_get_string(cfg, "cpu", "bg", NULL))) {
        g_strlcpy(cpu_st.bg, v, sizeof cpu_st.bg); g_free(v);
    }
    if ((v = g_key_file_get_string(cfg, "cpu", "label", NULL))) {
        g_strlcpy(cpu_st.label, v, sizeof cpu_st.label); g_free(v);
    }
    if ((v = g_key_file_get_string(cfg, "cpu", "image", NULL))) {
        g_strlcpy(cpu_st.bg_image, v, sizeof cpu_st.bg_image); g_free(v);
    }
}

/* Make sure cpu_bg matches cpu_st.bg_image (loads only if it changed). */
static int cpu_load_background(void)
{
    if (strcmp(cpu_bg_path, cpu_st.bg_image) == 0)
        return 0;
    animation_free(&cpu_bg);
    cpu_bg_path[0] = '\0';
    if (!cpu_st.bg_image[0])
        return 0;
    if (image_load(cpu_st.bg_image, &cpu_bg) != 0)
        return -1;
    g_strlcpy(cpu_bg_path, cpu_st.bg_image, sizeof cpu_bg_path);
    return 0;
}

/* Render the current temperature and hand it to the player. Call with `ctl` held. */
static int cpu_render_now(void)
{
    if (cpu_load_background() != 0)
        return -1;
    int t = cpu_temp_read();
    animation anim;
    if (render_cpu(&cpu_st, t, &cpu_bg, &anim) != 0)
        return -1;
    player_play(&anim);
    cpu_last = t;
    cpu_last_render = g_get_monotonic_time();
    return 0;
}

/* Strip control characters (TAB/newline would break the protocol). */
static void sanitize(char *s)
{
    for (; *s; s++)
        if ((unsigned char)*s < 0x20)
            *s = ' ';
}

/*
 * CPU [font=..<TAB>fg=#..<TAB>bg=#..<TAB>label=..]
 * Missing fields keep their current value.
 */
static char *do_cpu(const char *arg)
{
    cpu_style st = cpu_st;
    char **parts = g_strsplit(arg, "\t", -1);
    char *err = NULL;
    for (int i = 0; parts[i] && !err; i++) {
        char *eq = strchr(parts[i], '=');
        if (!eq)
            continue;
        *eq = '\0';
        const char *k = parts[i], *v = eq + 1;
        double rgb[3];
        if (strcmp(k, "font") == 0) {
            if (!v[0] || strlen(v) >= sizeof st.font)
                err = g_strdup("ERR invalid font name");
            else
                g_strlcpy(st.font, v, sizeof st.font);
        } else if (strcmp(k, "fg") == 0 || strcmp(k, "bg") == 0) {
            if (parse_hex_color(v, rgb) != 0)
                err = g_strdup_printf("ERR %s must be #rrggbb", k);
            else
                g_strlcpy(k[0] == 'f' ? st.fg : st.bg, v, sizeof st.fg);
        } else if (strcmp(k, "image") == 0) {
            /* "" removes the background; a bare name = library file. */
            char *abs = v[0] ? resolve_image(v) : g_strdup("");
            if (abs[0] && !g_file_test(abs, G_FILE_TEST_IS_REGULAR))
                err = g_strdup_printf("ERR background image not found: %s", v);
            else if (strlen(abs) >= sizeof st.bg_image)
                err = g_strdup("ERR background image path is too long");
            else
                g_strlcpy(st.bg_image, abs, sizeof st.bg_image);
            g_free(abs);
        } else if (strcmp(k, "label") == 0) {
            if (strlen(v) >= sizeof st.label)
                err = g_strdup_printf("ERR label is too long (max %zu bytes)", sizeof st.label - 1);
            else
                g_strlcpy(st.label, v, sizeof st.label);
        } else {
            err = g_strdup_printf("ERR unknown option: %s", k);
        }
    }
    g_strfreev(parts);
    if (err)
        return err;

    sanitize(st.font);
    sanitize(st.label);
    cpu_st = st;
    g_key_file_set_string(cfg, "cpu", "font", cpu_st.font);
    g_key_file_set_string(cfg, "cpu", "fg", cpu_st.fg);
    g_key_file_set_string(cfg, "cpu", "bg", cpu_st.bg);
    g_key_file_set_string(cfg, "cpu", "label", cpu_st.label);
    g_key_file_set_string(cfg, "cpu", "image", cpu_st.bg_image);
    g_key_file_set_string(cfg, "lcd", "mode", "cpu");
    cfg_save();
    if (cpu_render_now() != 0)
        return g_strdup_printf("ERR %s", image_error());
    return g_strdup("OK showing CPU temperature");
}

/* Every second: if the CPU screen is active and the temperature changed, re-render. */
static gboolean on_cpu_tick(gpointer unused)
{
    (void)unused;
    g_mutex_lock(&ctl);
    char *mode = cfg_str("mode", "none");
    if (strcmp(mode, "cpu") == 0 && cpu_temp_read() != cpu_last) {
        gint64 min = cpu_bg.count > 1 ? CPU_MIN_INTERVAL_ANIMATED : 0;
        if (g_get_monotonic_time() - cpu_last_render >= min)
            cpu_render_now();
    }
    g_free(mode);
    g_mutex_unlock(&ctl);
    return G_SOURCE_CONTINUE;
}

static char *do_show(const char *path)
{
    if (!g_path_is_absolute(path))
        return g_strdup("ERR path must be absolute");
    animation anim;
    if (image_load(path, &anim) != 0)
        return g_strdup_printf("ERR %s", image_error());
    int n = anim.count;
    player_play(&anim);
    g_key_file_set_string(cfg, "lcd", "mode", "image");
    g_key_file_set_string(cfg, "lcd", "image", path);
    cfg_save();
    char *base = g_path_get_basename(path);
    char *r = g_strdup_printf("OK showing %s (%d frame%s)", base, n, n == 1 ? "" : "s");
    g_free(base);
    return r;
}

static char *do_setting(const char *key, const char *arg, int hi, int (*fn)(int))
{
    int v;
    if (!parse_int(arg, 0, hi, &v))
        return g_strdup_printf("ERR value must be between 0 and %d", hi);
    int rc = fn(v);
    if (rc < 0)
        return g_strdup("ERR failed to send to the LCD");
    g_key_file_set_integer(cfg, "lcd", key, v);
    cfg_save();
    return g_strdup_printf("OK %s=%d%s", key, v,
                           rc == 1 ? " (LCD disconnected; will apply on reconnect)" : "");
}

static char *handle(const char *line)
{
    const char *sp = strchr(line, ' ');
    char *cmd = sp ? g_strndup(line, sp - line) : g_strdup(line);
    const char *arg = sp ? sp + 1 : "";
    char *r;

    if (strcmp(cmd, "PING") == 0) {
        r = g_strdup("OK pong");
    } else if (strcmp(cmd, "STATUS") == 0) {
        char *mode = cfg_str("mode", "none");
        char *img = cfg_str("image", "");
        char *abs = img[0] ? resolve_image(img) : g_strdup("");
        r = g_strdup_printf("OK\tconnected=%d\tplaying=%d\tmode=%s\tbrightness=%d\trotation=%d"
                            "\ttemp=%d\tcpu_font=%s\tcpu_fg=%s\tcpu_bg=%s\tcpu_label=%s\tcpu_image=%s\timage=%s",
                            player_connected(), player_playing(), mode,
                            cfg_int("brightness", 100), cfg_int("rotation", 0), cpu_temp_read(),
                            cpu_st.font, cpu_st.fg, cpu_st.bg, cpu_st.label, cpu_st.bg_image, abs);
        g_free(mode); g_free(img); g_free(abs);
    } else if (strcmp(cmd, "SHOW") == 0) {
        r = do_show(arg);
    } else if (strcmp(cmd, "CPU") == 0) {
        r = do_cpu(arg);
    } else if (strcmp(cmd, "BRIGHTNESS") == 0) {
        r = do_setting("brightness", arg, 100, player_set_brightness);
    } else if (strcmp(cmd, "ROTATION") == 0) {
        r = do_setting("rotation", arg, 3, player_set_rotation);
    } else if (strcmp(cmd, "STOP") == 0) {
        player_stop();
        g_key_file_set_string(cfg, "lcd", "mode", "none");
        cfg_save();
        r = g_strdup("OK stopped");
    } else if (strcmp(cmd, "HARDWARE") == 0) {
        int rc = player_hardware();
        g_key_file_set_string(cfg, "lcd", "mode", "hardware");
        cfg_save();
        r = g_strdup(rc == 0 ? "OK Corsair default screen"
                             : "ERR LCD disconnected; it returns to the default screen on its own");
    } else if (strcmp(cmd, "INFO") == 0) {
        char fw[32];
        r = player_firmware(fw, sizeof fw) == 0 ? g_strdup_printf("OK firmware %s", fw)
                                                : g_strdup("ERR LCD disconnected");
    } else {
        r = g_strdup_printf("ERR unknown command: %s", cmd);
    }
    g_free(cmd);
    return r;
}

/* Runs on a pool thread, one per connected client. */
static gboolean on_client(GThreadedSocketService *svc, GSocketConnection *conn,
                          GObject *src, gpointer u)
{
    (void)svc; (void)src; (void)u;
    GDataInputStream *in = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    char *line;
    while ((line = g_data_input_stream_read_line_utf8(in, NULL, NULL, NULL))) {
        g_mutex_lock(&ctl);
        char *reply = handle(g_strstrip(line));
        g_mutex_unlock(&ctl);
        char *msg = g_strconcat(reply, "\n", NULL);
        gboolean ok = g_output_stream_write_all(out, msg, strlen(msg), NULL, NULL, NULL);
        g_free(msg);
        g_free(reply);
        g_free(line);
        if (!ok)
            break;
    }
    g_object_unref(in);
    return TRUE;
}

/* Restore whatever was being shown before the service stopped. */
static void restore_state(void)
{
    char *mode = cfg_str("mode", "none");
    if (strcmp(mode, "image") == 0) {
        char *img = cfg_str("image", "");
        if (img[0]) {
            char *abs = resolve_image(img);
            char *r = do_show(abs);
            g_message("restoring image: %s", r);
            g_free(r);
            g_free(abs);
        }
        g_free(img);
    } else if (strcmp(mode, "cpu") == 0) {
        cpu_render_now();
        g_message("restoring CPU temperature screen");
    }
    g_free(mode);
}

static gboolean on_quit(gpointer loop)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

int main(void)
{
    char *sock = ipc_socket_path();

    /* Is a service already answering? Then don't start another one. */
    char *pong = ipc_request("PING", NULL);
    if (pong) {
        g_printerr("nautilusctld is already running (%s)\n", sock);
        g_free(pong);
        g_free(sock);
        return 1;
    }
    g_unlink(sock);                            /* stale socket from a previous run */

    lib_dir = g_build_filename(g_get_user_data_dir(), "nautilusctl", "library", NULL);
    g_mkdir_with_parents(lib_dir, 0755);
    cfg_path = g_build_filename(g_get_user_config_dir(), "nautilusctl", "config.ini", NULL);
    cfg = g_key_file_new();
    g_key_file_load_from_file(cfg, cfg_path, G_KEY_FILE_KEEP_COMMENTS, NULL);

    g_mutex_init(&ctl);
    player_init(cfg_int("brightness", 100), cfg_int("rotation", 0));
    cpu_load_style();

    GSocketService *svc = g_threaded_socket_service_new(4);
    GSocketAddress *addr = g_unix_socket_address_new(sock);
    GError *err = NULL;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(svc), addr, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &err)) {
        g_printerr("could not create %s: %s\n", sock, err->message);
        g_error_free(err);
        player_shutdown();
        g_free(sock);
        return 1;
    }
    g_chmod(sock, 0600);                       /* only the owning user can talk to the service */
    g_signal_connect(svc, "run", G_CALLBACK(on_client), NULL);

    g_mutex_lock(&ctl);
    restore_state();
    g_mutex_unlock(&ctl);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(1, on_cpu_tick, NULL);
    g_unix_signal_add(SIGTERM, on_quit, loop);
    g_unix_signal_add(SIGINT, on_quit, loop);
    g_message("nautilusctld ready on %s", sock);
    g_main_loop_run(loop);

    g_message("shutting down");
    g_socket_service_stop(svc);
    g_socket_listener_close(G_SOCKET_LISTENER(svc));
    player_shutdown();
    animation_free(&cpu_bg);
    g_unlink(sock);
    g_object_unref(addr);
    g_object_unref(svc);
    g_main_loop_unref(loop);
    g_key_file_free(cfg);
    g_free(cfg_path);
    g_free(lib_dir);
    g_free(sock);
    return 0;
}
