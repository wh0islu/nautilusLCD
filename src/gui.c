/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * gui.c — graphical interface (GTK 4) for nautilusctl.
 *
 * The window is a REMOTE CONTROL: the nautilusctld service is what talks
 * to the LCD. Every action becomes a command on the Unix socket (ipc.c).
 * Closing the window does not affect what is on the cooler's screen.
 *
 * Threads:
 *   - main thread: GTK event loop (the only one that touches widgets);
 *   - background GTask: talks to the service and decodes the preview,
 *     so the window does not freeze on large GIFs.
 *
 * Every 2 s the window asks the service for STATUS and syncs itself,
 * so it also reflects changes made from the CLI.
 */
#include "image.h"
#include "ipc.h"
#include "render.h"
#include "sensors.h"

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <string.h>

#define APP_ID "dev.nautilusctl.Gui"
#define PREVIEW_SIZE 240
#define THUMB_SIZE 64
#define POLL_MS 2000

enum { ST_IDLE, ST_OK, ST_ERR };

typedef struct {
    /* widgets */
    GtkWidget *win, *dot, *status, *preview, *flow, *empty, *bright, *bright_val;
    GtkWidget *rot[4], *start_btn;
    GtkWidget *cpu_font, *cpu_fg, *cpu_label, *cpu_switch;
    GtkWidget *cpu_bg;          /* solid color button (next to "Default screen") */
    char *cur_image;            /* current background: library image, or NULL = solid color */
    gboolean cpu_active;        /* the service is showing the CPU screen */
    guint cpu_timer;

    /* animated preview */
    GPtrArray *textures;        /* GdkTexture* */
    GArray *delays;             /* int, ms */
    guint preview_idx, preview_timer;
    char *preview_key;          /* path of the image in the current preview */

    /* state */
    GHashTable *thumbs;         /* name -> GdkTexture (thumbnail cache) */
    char *lib_dir;
    guint gen;                  /* user actions; discards stale replies */
    gboolean synced;            /* already received the first STATUS? */
    gboolean applying;          /* avoid reacting to changes made by the code itself */
    gboolean busy;              /* a command is in flight */
    gboolean poll_pending;
    guint bright_timer;
    char *flash;                /* temporary message (action error/success) */
    gint64 flash_until;
} App;

static App A;

/* ================================================================== */
/* Style                                                               */
/* ================================================================== */

static const char *CSS =
    "window, window.background { background: #0d0e12; color: #e7e9ef; }"
    "headerbar { background: #0d0e12; border: none; box-shadow: none; min-height: 46px; }"
    "headerbar .title { font-weight: 700; font-size: 14px; letter-spacing: 0.5px; }"
    ".dot { min-width: 8px; min-height: 8px; border-radius: 99px; background: #4a4f5c; }"
    ".dot.ok { background: #3ddc97; box-shadow: 0 0 8px rgba(61,220,151,0.8); }"
    ".dot.err { background: #ff5c6c; box-shadow: 0 0 8px rgba(255,92,108,0.7); }"
    ".lcd { border-radius: 9999px; background: #000;"
    "       box-shadow: 0 0 0 3px #1d2029, 0 0 0 9px #13151b, 0 0 60px rgba(90,169,255,0.18); }"
    ".status { color: #8b92a5; font-size: 12px; }"
    ".card { background: #14161c; border-radius: 16px; padding: 16px; }"
    ".section { color: #6f7689; font-size: 10px; font-weight: 800; letter-spacing: 1.5px; }"
    ".muted { color: #6f7689; font-size: 12px; }"
    ".value { color: #e7e9ef; font-size: 12px; font-weight: 700; min-width: 34px; }"
    "button.pill { background: #1d2029; color: #e7e9ef; border: none; box-shadow: none;"
    "              border-radius: 999px; padding: 6px 16px; font-weight: 600; }"
    "button.pill:hover { background: #262a35; }"
    "button.pill.accent { background: #5aa9ff; color: #0b0c10; }"
    "button.pill.accent:hover { background: #7bbbff; }"
    "button.pill.small { padding: 3px 12px; font-size: 12px; }"
    "button.pill.danger:hover { background: #3a1e24; color: #ff8a96; }"
    "button.seg { background: #1d2029; color: #aab0c0; border: none; box-shadow: none;"
    "             border-radius: 10px; padding: 6px 0; font-weight: 600; }"
    "button.seg:hover { background: #262a35; }"
    "button.seg:checked { background: #5aa9ff; color: #0b0c10; }"
    "flowbox { background: none; }"
    "flowboxchild { padding: 5px; border-radius: 999px; background: none; outline: none; }"
    "flowboxchild:hover .thumb { box-shadow: 0 0 0 2px #3a4050; }"
    "flowboxchild:selected { background: none; }"
    "flowboxchild:selected .thumb { box-shadow: 0 0 0 2px #5aa9ff, 0 0 14px rgba(90,169,255,0.5); }"
    ".thumb { border-radius: 9999px; background: #000; }"
    "scale { padding: 8px 0; }"
    "scale trough { background: #23262f; min-height: 6px; border-radius: 99px; border: none;"
    "               padding: 0; outline: none; }"
    "scale highlight { background: #5aa9ff; border-radius: 99px; border: none;"
    "                  margin: 0; padding: 0; min-height: 6px; min-width: 0; }"
    "scale slider { background: #e7e9ef; min-width: 16px; min-height: 16px; margin: -5px 0;"
    "               border-radius: 99px; border: none; outline: none;"
    "               box-shadow: 0 1px 4px rgba(0,0,0,0.5); }"
    ".field { color: #aab0c0; font-size: 12px; }"
    "entry.dark { background: #1d2029; color: #e7e9ef; border: none; box-shadow: none; outline: none;"
    "             border-radius: 10px; padding: 0 10px; min-height: 30px; }"
    ".picker > button { background: #1d2029; color: #e7e9ef; border: none; box-shadow: none;"
    "                   border-radius: 10px; padding: 4px 10px; min-height: 22px; }"
    ".picker > button:hover { background: #262a35; }"
    ".swatch-btn > button { background: #1d2029; border: none; box-shadow: none;"
    "                       border-radius: 999px; padding: 5px; min-width: 0; min-height: 0; }"
    ".swatch-btn > button:hover { background: #262a35; }"
    ".swatch-btn colorswatch { border-radius: 999px; min-width: 22px; min-height: 22px; }"
    "switch { background: #23262f; border: none; box-shadow: none; }"
    "switch:checked { background: #5aa9ff; }"
    "switch slider { background: #e7e9ef; border: none; box-shadow: none; }"
    "scrollbar { background: none; border: none; }";

/* ================================================================== */
/* Utilities                                                           */
/* ================================================================== */

static void set_status(const char *msg, int state)
{
    gtk_label_set_text(GTK_LABEL(A.status), msg);
    gtk_widget_remove_css_class(A.dot, "ok");
    gtk_widget_remove_css_class(A.dot, "err");
    if (state == ST_OK)
        gtk_widget_add_css_class(A.dot, "ok");
    else if (state == ST_ERR)
        gtk_widget_add_css_class(A.dot, "err");
}

/* Show a message for a few seconds, before the next STATUS. */
static void flash(const char *msg, int state)
{
    g_free(A.flash);
    A.flash = g_strdup(msg);
    A.flash_until = g_get_monotonic_time() + 4 * G_TIME_SPAN_SECOND;
    set_status(msg, state);
}

static GtkWidget *label_new(const char *text, const char *css)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    if (css)
        gtk_widget_add_css_class(l, css);
    return l;
}

static GtkWidget *pill(const char *text, const char *extra)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    gtk_widget_add_css_class(b, "pill");
    if (extra)
        gtk_widget_add_css_class(b, extra);
    return b;
}

/* "OK text" -> "text"; "ERR text" -> "text" */
static const char *reply_text(const char *r)
{
    const char *t = r + (g_str_has_prefix(r, "OK") ? 2 : g_str_has_prefix(r, "ERR") ? 3 : 0);
    while (*t == ' ' || *t == '\t')
        t++;
    return t;
}

/* ================================================================== */
/* Round preview                                                       */
/* ================================================================== */

static void preview_stop(void)
{
    if (A.preview_timer) {
        g_source_remove(A.preview_timer);
        A.preview_timer = 0;
    }
}

static gboolean preview_tick(gpointer unused)
{
    (void)unused;
    A.preview_timer = 0;
    if (A.textures->len == 0)
        return G_SOURCE_REMOVE;
    A.preview_idx = (A.preview_idx + 1) % A.textures->len;
    gtk_picture_set_paintable(GTK_PICTURE(A.preview), g_ptr_array_index(A.textures, A.preview_idx));
    A.preview_timer = g_timeout_add(g_array_index(A.delays, int, A.preview_idx), preview_tick, NULL);
    return G_SOURCE_REMOVE;
}

static void preview_clear(const char *key)
{
    preview_stop();
    gtk_picture_set_paintable(GTK_PICTURE(A.preview), NULL);
    g_ptr_array_set_size(A.textures, 0);
    g_array_set_size(A.delays, 0);
    g_free(A.preview_key);
    A.preview_key = g_strdup(key ? key : "");
}

static void preview_set(const animation *anim, const char *key)
{
    preview_clear(key);
    for (int i = 0; i < anim->count; i++) {
        GBytes *b = g_bytes_new(anim->frames[i].jpeg, anim->frames[i].size);
        GdkTexture *t = gdk_texture_new_from_bytes(b, NULL);
        g_bytes_unref(b);
        if (!t)
            continue;
        int d = anim->frames[i].delay_ms > 0 ? anim->frames[i].delay_ms : 100;
        g_ptr_array_add(A.textures, t);
        g_array_append_val(A.delays, d);
    }
    if (A.textures->len == 0)
        return;
    A.preview_idx = 0;
    gtk_picture_set_paintable(GTK_PICTURE(A.preview), g_ptr_array_index(A.textures, 0));
    if (A.textures->len > 1)
        A.preview_timer = g_timeout_add(g_array_index(A.delays, int, 0), preview_tick, NULL);
}

/* ================================================================== */
/* Background tasks: command to the service + preview decoding         */
/* ================================================================== */

typedef enum { JOB_ACTION, JOB_PREVIEW, JOB_POLL } JobKind;

typedef struct {
    JobKind kind;
    guint gen;
    char *command;        /* command for the service (or NULL) */
    char *preview_path;   /* decode this image for the preview */
    gboolean cpu;         /* or render the CPU screen with `style`/`temp` */
    cpu_style style;
    int temp;
    char *key;            /* preview identifier */
    animation anim;
    char *reply, *error;
} Job;

static void job_free(gpointer p)
{
    Job *j = p;
    animation_free(&j->anim);
    g_free(j->command);
    g_free(j->preview_path);
    g_free(j->key);
    g_free(j->reply);
    g_free(j->error);
    g_free(j);
}

static void job_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src;
    (void)c;
    Job *j = data;
    if (j->cpu) {
        /* Same code as the service; with a GIF background it takes ~1 s, hence here. */
        animation bg = { NULL, 0 };
        if (j->style.bg_image[0] && image_load(j->style.bg_image, &bg) != 0)
            j->error = g_strdup(image_error());
        else if (render_cpu(&j->style, j->temp, &bg, &j->anim) != 0)
            j->error = g_strdup(image_error());
        animation_free(&bg);
    } else if (j->preview_path) {
        if (image_load(j->preview_path, &j->anim) != 0)
            j->error = g_strdup(image_error());
    }
    if (j->command && !j->error) {
        GError *e = NULL;
        j->reply = ipc_request(j->command, &e);
        if (!j->reply) {
            j->error = g_strdup_printf("Service unavailable: %s", e->message);
            g_error_free(e);
        }
    }
    g_task_return_boolean(task, TRUE);
}

static void on_job_done(GObject *src, GAsyncResult *res, gpointer unused);

static void run_job(Job *j)
{
    GTask *task = g_task_new(NULL, NULL, on_job_done, NULL);
    g_task_set_task_data(task, j, job_free);
    g_task_run_in_thread(task, job_thread);
    g_object_unref(task);
}

/* User action: command + (optional) preview. */
static void action(char *command, char *preview_path, const char *key)
{
    Job *j = g_new0(Job, 1);
    j->kind = JOB_ACTION;
    j->gen = ++A.gen;
    j->command = command;
    j->preview_path = preview_path;
    j->key = g_strdup(key);
    A.busy = TRUE;
    run_job(j);
}

/* Just a command, no preview (brightness, rotation...). */
static void command(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void command(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Job *j = g_new0(Job, 1);
    j->kind = JOB_ACTION;
    j->gen = A.gen;            /* does not invalidate image actions in flight */
    j->command = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    run_job(j);
}

/* ================================================================== */
/* Sync with the service (STATUS)                                      */
/* ================================================================== */

static void library_select(const char *path);

/*
 * "Sans Bold" or "JetBrains Mono" may be aliases/approximations that
 * fontconfig resolves, but GTK's font button only knows real families.
 * We load the font and ask which one was actually picked.
 */
static void set_font_button(const char *desc)
{
    PangoFontDescription *want = pango_font_description_from_string(desc);
    pango_font_description_set_size(want, 14 * PANGO_SCALE);
    PangoFontMap *fm = pango_cairo_font_map_get_default();
    PangoContext *ctx = pango_font_map_create_context(fm);
    PangoFont *font = pango_context_load_font(ctx, want);
    PangoFontDescription *real = font ? pango_font_describe(font) : pango_font_description_copy(want);
    pango_font_description_set_size(real, 14 * PANGO_SCALE);
    gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(A.cpu_font), real);
    pango_font_description_free(real);
    if (font)
        g_object_unref(font);
    g_object_unref(ctx);
    pango_font_description_free(want);
}

static void rgba_to_hex(const GdkRGBA *c, char hex[8])
{
    g_snprintf(hex, 8, "#%02x%02x%02x", (int)(c->red * 255 + 0.5),
               (int)(c->green * 255 + 0.5), (int)(c->blue * 255 + 0.5));
}

/* Read the style from the CPU TEMPERATURE card controls. */
static void cpu_style_from_widgets(cpu_style *st)
{
    cpu_style_default(st);
    PangoFontDescription *fd =
        gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(A.cpu_font));
    if (fd) {
        /* Family/style only: the size is decided by the renderer. */
        PangoFontDescription *copy = pango_font_description_copy(fd);
        pango_font_description_unset_fields(copy, PANGO_FONT_MASK_SIZE);
        char *str = pango_font_description_to_string(copy);
        g_strlcpy(st->font, str, sizeof st->font);
        g_free(str);
        pango_font_description_free(copy);
    }
    rgba_to_hex(gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(A.cpu_fg)), st->fg);
    rgba_to_hex(gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(A.cpu_bg)), st->bg);
    g_strlcpy(st->label, gtk_editable_get_text(GTK_EDITABLE(A.cpu_label)), sizeof st->label);
    if (A.cur_image)
        g_strlcpy(st->bg_image, A.cur_image, sizeof st->bg_image);
}

static void set_cur_image(const char *path)
{
    g_free(A.cur_image);
    A.cur_image = path && path[0] ? g_strdup(path) : NULL;
}

/* Render the CPU screen preview with the same code as the service (render.c). */
static void run_job(Job *j);

static void cpu_preview(const cpu_style *st, int temp)
{
    char *key = g_strdup_printf("cpu|%s|%s|%s|%s|%s|%d", st->font, st->fg, st->bg,
                                st->label, st->bg_image, temp);
    if (g_strcmp0(key, A.preview_key) == 0) {   /* nothing changed: don't re-render */
        g_free(key);
        return;
    }
    g_free(A.preview_key);
    A.preview_key = g_strdup(key);             /* avoid triggering again */
    Job *j = g_new0(Job, 1);
    j->kind = JOB_PREVIEW;
    j->cpu = TRUE;
    j->style = *st;
    j->temp = temp;
    j->key = key;
    run_job(j);
}

static void apply_status(GHashTable *s)
{
    const char *mode = g_hash_table_lookup(s, "mode");
    const char *image = g_hash_table_lookup(s, "image");
    gboolean connected = g_strcmp0(g_hash_table_lookup(s, "connected"), "1") == 0;
    gboolean playing = g_strcmp0(g_hash_table_lookup(s, "playing"), "1") == 0;

    /* Controls: only on the first STATUS, so we don't fight the user. */
    if (!A.synced) {
        A.synced = TRUE;
        A.applying = TRUE;
        const char *b = g_hash_table_lookup(s, "brightness");
        const char *r = g_hash_table_lookup(s, "rotation");
        if (b)
            gtk_range_set_value(GTK_RANGE(A.bright), CLAMP(atoi(b), 0, 100));
        if (r)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(A.rot[CLAMP(atoi(r), 0, 3)]), TRUE);

        const char *f = g_hash_table_lookup(s, "cpu_font");
        if (f && f[0])
            set_font_button(f);
        GdkRGBA c;
        if (gdk_rgba_parse(&c, g_hash_table_lookup(s, "cpu_fg") ?: ""))
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(A.cpu_fg), &c);
        if (gdk_rgba_parse(&c, g_hash_table_lookup(s, "cpu_bg") ?: ""))
            gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(A.cpu_bg), &c);
        const char *l = g_hash_table_lookup(s, "cpu_label");
        gtk_editable_set_text(GTK_EDITABLE(A.cpu_label), l ? l : "");
        A.applying = FALSE;
    }

    /* Current background and switch follow the service (including via CLI). */
    A.cpu_active = playing && g_strcmp0(mode, "cpu") == 0;
    if (!A.busy) {
        if (g_strcmp0(mode, "image") == 0)
            set_cur_image(image);
        else if (g_strcmp0(mode, "cpu") == 0)
            set_cur_image(g_hash_table_lookup(s, "cpu_image"));
        A.applying = TRUE;
        if (g_strcmp0(mode, "image") == 0 || g_strcmp0(mode, "cpu") == 0)
            gtk_switch_set_active(GTK_SWITCH(A.cpu_switch), g_strcmp0(mode, "cpu") == 0);
        A.applying = FALSE;
        /* A solid color always shows the temperature: the switch is locked. */
        gtk_widget_set_sensitive(A.cpu_switch, !(A.cpu_active && !A.cur_image));
    }
    if (A.cpu_active && !A.busy) {
        cpu_style st;
        cpu_style_default(&st);
        const char *v;
        if ((v = g_hash_table_lookup(s, "cpu_font"))) g_strlcpy(st.font, v, sizeof st.font);
        if ((v = g_hash_table_lookup(s, "cpu_fg"))) g_strlcpy(st.fg, v, sizeof st.fg);
        if ((v = g_hash_table_lookup(s, "cpu_bg"))) g_strlcpy(st.bg, v, sizeof st.bg);
        if ((v = g_hash_table_lookup(s, "cpu_label"))) g_strlcpy(st.label, v, sizeof st.label);
        if ((v = g_hash_table_lookup(s, "cpu_image"))) g_strlcpy(st.bg_image, v, sizeof st.bg_image);
        v = g_hash_table_lookup(s, "temp");
        cpu_preview(&st, v ? atoi(v) : CPU_TEMP_UNAVAILABLE);
        library_select(st.bg_image[0] ? st.bg_image : NULL);
    }

    /* Image preview: follows what the service shows (including via CLI). */
    const char *key = "";
    if (playing && g_strcmp0(mode, "image") == 0 && image)
        key = image;
    if (!A.cpu_active && !A.busy && g_strcmp0(key, A.preview_key) != 0) {
        if (!key[0]) {
            preview_clear("");
        } else {
            Job *j = g_new0(Job, 1);
            j->kind = JOB_PREVIEW;
            j->gen = A.gen;
            j->key = g_strdup(key);
            j->preview_path = g_strdup(key);
            g_free(A.preview_key);
            A.preview_key = g_strdup(key);       /* avoid triggering again */
            run_job(j);
        }
        if (g_strcmp0(mode, "image") == 0)
            library_select(image);
        else
            gtk_flow_box_unselect_all(GTK_FLOW_BOX(A.flow));
    }

    /* Status text (unless there is a recent action message). */
    if (A.flash && g_get_monotonic_time() < A.flash_until)
        return;
    g_clear_pointer(&A.flash, g_free);

    /* State only, no file name. */
    if (!connected)
        set_status("LCD disconnected — reconnecting…", ST_ERR);
    else if (playing)
        set_status("Live on LCD", ST_OK);
    else if (g_strcmp0(mode, "hardware") == 0)
        set_status("Corsair default screen", ST_OK);
    else
        set_status("Stopped", ST_IDLE);
}

static gboolean poll(gpointer unused);

static void on_job_done(GObject *src, GAsyncResult *res, gpointer unused)
{
    (void)src;
    (void)unused;
    Job *j = g_task_get_task_data(G_TASK(res));

    if (j->kind == JOB_POLL) {
        A.poll_pending = FALSE;
        gboolean up = j->reply != NULL;
        gtk_widget_set_visible(A.start_btn, !up);
        if (!up) {
            A.synced = FALSE;                    /* resync when the service comes back */
            set_status("Service is not running", ST_ERR);
            return;
        }
        GHashTable *s = ipc_parse_status(j->reply);
        apply_status(s);
        g_hash_table_unref(s);
        return;
    }

    if (j->kind == JOB_PREVIEW) {
        if (!j->error && g_strcmp0(j->key, A.preview_key) == 0)
            preview_set(&j->anim, j->key);
        return;
    }

    /* JOB_ACTION */
    if (j->gen == A.gen)
        A.busy = FALSE;
    if (j->error) {
        flash(j->error, ST_ERR);
        return;
    }
    if (!g_str_has_prefix(j->reply, "OK")) {
        flash(reply_text(j->reply), ST_ERR);
        return;
    }
    if (j->gen == A.gen && j->key && j->anim.count > 0)
        preview_set(&j->anim, j->key);
    /* Success: the status text comes from STATUS (no file name). */
    g_clear_pointer(&A.flash, g_free);
    poll(NULL);
}

static gboolean poll(gpointer unused)
{
    (void)unused;
    if (!A.poll_pending) {
        A.poll_pending = TRUE;
        Job *j = g_new0(Job, 1);
        j->kind = JOB_POLL;
        j->command = g_strdup("STATUS");
        run_job(j);
    }
    return G_SOURCE_CONTINUE;
}

/* ================================================================== */
/* Library                                                             */
/* ================================================================== */

static gboolean is_supported(const char *name)
{
    char *lower = g_ascii_strdown(name, -1);
    gboolean ok = g_str_has_suffix(lower, ".jpg") || g_str_has_suffix(lower, ".jpeg") ||
                  g_str_has_suffix(lower, ".png") || g_str_has_suffix(lower, ".gif");
    g_free(lower);
    return ok;
}

/* Thumbnail through the same pipeline as the LCD (first frame), cached. */
static GdkTexture *thumb_texture(const char *name)
{
    GdkTexture *t = g_hash_table_lookup(A.thumbs, name);
    if (t)
        return t;
    char *path = g_build_filename(A.lib_dir, name, NULL);
    animation anim;
    if (image_load_first(path, &anim) == 0 && anim.count > 0) {
        GBytes *b = g_bytes_new(anim.frames[0].jpeg, anim.frames[0].size);
        t = gdk_texture_new_from_bytes(b, NULL);
        g_bytes_unref(b);
        animation_free(&anim);
    }
    g_free(path);
    if (t)
        g_hash_table_insert(A.thumbs, g_strdup(name), t);
    return t;
}

static GtkWidget *make_thumb(const char *name)
{
    GtkWidget *circle = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(circle, "thumb");
    gtk_widget_set_overflow(circle, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(circle, THUMB_SIZE, THUMB_SIZE);
    gtk_widget_set_halign(circle, GTK_ALIGN_CENTER);

    GtkWidget *pic = gtk_picture_new_for_paintable(GDK_PAINTABLE(thumb_texture(name)));
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_hexpand(pic, TRUE);
    gtk_widget_set_vexpand(pic, TRUE);
    gtk_box_append(GTK_BOX(circle), pic);

    g_object_set_data_full(G_OBJECT(circle), "name", g_strdup(name), g_free);
    return circle;
}

static gint cmp_str(gconstpointer a, gconstpointer b)
{
    return g_utf8_collate(*(const char *const *)a, *(const char *const *)b);
}

static void library_refresh(void)
{
    gtk_flow_box_remove_all(GTK_FLOW_BOX(A.flow));
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GDir *dir = g_dir_open(A.lib_dir, 0, NULL);
    if (dir) {
        const char *n;
        while ((n = g_dir_read_name(dir)))
            if (is_supported(n))
                g_ptr_array_add(names, g_strdup(n));
        g_dir_close(dir);
    }
    g_ptr_array_sort(names, cmp_str);
    for (guint i = 0; i < names->len; i++)
        gtk_flow_box_append(GTK_FLOW_BOX(A.flow), make_thumb(g_ptr_array_index(names, i)));

    gtk_widget_set_visible(A.empty, names->len == 0);
    gtk_widget_set_visible(A.flow, names->len > 0);
    g_ptr_array_unref(names);
}

/* Highlight the thumbnail whose path is `path` (if it is in the library). */
static void library_select(const char *path)
{
    gtk_flow_box_unselect_all(GTK_FLOW_BOX(A.flow));
    if (!path)
        return;
    char *dir = g_path_get_dirname(path);
    char *base = g_path_get_basename(path);
    if (g_strcmp0(dir, A.lib_dir) == 0) {
        for (int i = 0;; i++) {
            GtkFlowBoxChild *c = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(A.flow), i);
            if (!c)
                break;
            const char *n = g_object_get_data(G_OBJECT(gtk_flow_box_child_get_child(c)), "name");
            if (g_strcmp0(n, base) == 0) {
                gtk_flow_box_select_child(GTK_FLOW_BOX(A.flow), c);
                break;
            }
        }
    }
    g_free(dir);
    g_free(base);
}

static void send_cpu(void);

static void show_image(const char *path)
{
    set_status("Loading…", ST_IDLE);
    action(g_strdup_printf("SHOW %s", path), g_strdup(path), path);
}

static void show_library_item(const char *name)
{
    char *path = g_build_filename(A.lib_dir, name, NULL);
    set_cur_image(path);
    if (gtk_switch_get_active(GTK_SWITCH(A.cpu_switch)))
        send_cpu();                              /* temperature over the image */
    else
        show_image(path);
    g_free(path);
}

static void on_thumb_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer u)
{
    (void)box;
    (void)u;
    const char *name = g_object_get_data(G_OBJECT(gtk_flow_box_child_get_child(child)), "name");
    if (name)
        show_library_item(name);
}

static char *unique_dest(const char *base)
{
    char *dest = g_build_filename(A.lib_dir, base, NULL);
    const char *dot = strrchr(base, '.');
    char *stem = g_strndup(base, dot ? (gsize)(dot - base) : strlen(base));
    for (int i = 2; g_file_test(dest, G_FILE_TEST_EXISTS); i++) {
        g_free(dest);
        char *n = g_strdup_printf("%s-%d%s", stem, i, dot ? dot : "");
        dest = g_build_filename(A.lib_dir, n, NULL);
        g_free(n);
    }
    g_free(stem);
    return dest;
}

static void on_files_chosen(GObject *src, GAsyncResult *res, gpointer u)
{
    (void)u;
    GListModel *files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(src), res, NULL);
    if (!files)
        return;

    char *last = NULL;
    guint n = g_list_model_get_n_items(files), added = 0;
    for (guint i = 0; i < n; i++) {
        GFile *f = g_list_model_get_item(files, i);
        char *base = g_file_get_basename(f);
        if (is_supported(base)) {
            char *dest = unique_dest(base);
            GFile *df = g_file_new_for_path(dest);
            GError *err = NULL;
            if (g_file_copy(f, df, G_FILE_COPY_NONE, NULL, NULL, NULL, &err)) {
                g_free(last);
                last = g_path_get_basename(dest);
                added++;
            } else {
                flash(err->message, ST_ERR);
                g_error_free(err);
            }
            g_object_unref(df);
            g_free(dest);
        }
        g_free(base);
        g_object_unref(f);
    }
    g_object_unref(files);

    library_refresh();
    if (last && added == 1)
        show_library_item(last);                 /* a single file: show it right away */
    g_free(last);
}

static void on_add(GtkButton *b, gpointer u)
{
    (void)b;
    (void)u;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Add images or GIFs");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Images (JPG, PNG, GIF)");
    gtk_file_filter_add_mime_type(filter, "image/jpeg");
    gtk_file_filter_add_mime_type(filter, "image/png");
    gtk_file_filter_add_mime_type(filter, "image/gif");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open_multiple(dlg, GTK_WINDOW(A.win), NULL, on_files_chosen, NULL);
    g_object_unref(filters);
    g_object_unref(filter);
    g_object_unref(dlg);
}

static void on_remove(GtkButton *b, gpointer u)
{
    (void)b;
    (void)u;
    GList *sel = gtk_flow_box_get_selected_children(GTK_FLOW_BOX(A.flow));
    if (!sel) {
        flash("Select an image to remove", ST_IDLE);
        return;
    }
    for (GList *l = sel; l; l = l->next) {
        const char *name = g_object_get_data(G_OBJECT(gtk_flow_box_child_get_child(l->data)), "name");
        char *path = g_build_filename(A.lib_dir, name, NULL);
        /* The service already has the frames in memory: the screen stays until changed. */
        g_unlink(path);
        g_free(path);
        g_hash_table_remove(A.thumbs, name);
    }
    g_list_free(sel);
    library_refresh();
}

/* ================================================================== */
/* Color, brightness, rotation, stop, default screen, start service    */
/* ================================================================== */

static gboolean send_brightness(gpointer u)
{
    (void)u;
    A.bright_timer = 0;
    command("BRIGHTNESS %d", (int)gtk_range_get_value(GTK_RANGE(A.bright)));
    return G_SOURCE_REMOVE;
}

static void on_brightness(GtkRange *r, gpointer u)
{
    (void)u;
    char txt[8];
    g_snprintf(txt, sizeof txt, "%d%%", (int)gtk_range_get_value(r));
    gtk_label_set_text(GTK_LABEL(A.bright_val), txt);
    if (A.applying)
        return;
    /* debounce: dragging fires dozens of events; send only when it stops */
    if (A.bright_timer)
        g_source_remove(A.bright_timer);
    A.bright_timer = g_timeout_add(120, send_brightness, NULL);
}

static void on_rotation(GtkToggleButton *t, gpointer data)
{
    if (A.applying || !gtk_toggle_button_get_active(t))
        return;
    command("ROTATION %d", GPOINTER_TO_INT(data));
}

/* Build "CPU font=..<TAB>fg=..<TAB>bg=..<TAB>label=.." and send it. */
static void send_cpu(void)
{
    cpu_style st;
    cpu_style_from_widgets(&st);
    for (char *p = st.label; *p; p++)        /* a TAB would break the protocol */
        if ((unsigned char)*p < 0x20)
            *p = ' ';
    library_select(A.cur_image);
    gtk_widget_set_sensitive(A.cpu_switch, A.cur_image != NULL);
    cpu_preview(&st, cpu_temp_read());       /* immediate visual feedback */
    action(g_strdup_printf("CPU font=%s\tfg=%s\tbg=%s\tlabel=%s\timage=%s",
                           st.font, st.fg, st.bg, st.label, st.bg_image),
           NULL, NULL);
}

/* "Temperature on top" switch: turns it on/off over the current background. */
static void on_cpu_switch(GObject *o, GParamSpec *ps, gpointer u)
{
    (void)ps;
    (void)u;
    if (A.applying)
        return;
    if (gtk_switch_get_active(GTK_SWITCH(o)))
        send_cpu();
    else if (A.cur_image)
        show_image(A.cur_image);
}

/* Color button: solid color background, always with the temperature. */
static void on_solid_color(GObject *o, GParamSpec *ps, gpointer u)
{
    (void)o;
    (void)ps;
    (void)u;
    if (A.applying)
        return;
    set_cur_image(NULL);
    A.applying = TRUE;
    gtk_switch_set_active(GTK_SWITCH(A.cpu_switch), TRUE);
    A.applying = FALSE;
    send_cpu();
}

static gboolean cpu_debounced(gpointer u)
{
    (void)u;
    A.cpu_timer = 0;
    send_cpu();
    return G_SOURCE_REMOVE;
}

/* Font/color/label changed: if the CPU screen is on the LCD, apply it live. */
static void on_cpu_style_changed(void)
{
    if (A.applying || !A.cpu_active)
        return;
    if (A.cpu_timer)
        g_source_remove(A.cpu_timer);
    A.cpu_timer = g_timeout_add(300, cpu_debounced, NULL);
}

static void on_cpu_notify(GObject *o, GParamSpec *ps, gpointer u)
{
    (void)o;
    (void)ps;
    (void)u;
    on_cpu_style_changed();
}

static void on_cpu_label_changed(GtkEditable *e, gpointer u)
{
    (void)e;
    (void)u;
    on_cpu_style_changed();
}

static void on_stop(GtkButton *b, gpointer u)
{
    (void)b;
    (void)u;
    preview_clear("");
    gtk_flow_box_unselect_all(GTK_FLOW_BOX(A.flow));
    action(g_strdup("STOP"), NULL, NULL);
}

static void on_hardware(GtkButton *b, gpointer u)
{
    (void)b;
    (void)u;
    preview_clear("");
    gtk_flow_box_unselect_all(GTK_FLOW_BOX(A.flow));
    action(g_strdup("HARDWARE"), NULL, NULL);
}

static void on_start_service(GtkButton *b, gpointer u)
{
    (void)b;
    (void)u;
    GError *e = NULL;
    if (!g_spawn_command_line_async("systemctl --user start nautilusctld.service", &e)) {
        flash(e->message, ST_ERR);
        g_error_free(e);
        return;
    }
    flash("Starting the service…", ST_IDLE);
}

/* ================================================================== */
/* Window layout                                                       */
/* ================================================================== */

static GtkWidget *card(void)
{
    GtkWidget *c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_add_css_class(c, "card");
    return c;
}

static GtkWidget *row(void)
{
    return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
}

static GtkWidget *spacer(void)
{
    GtkWidget *s = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(s, TRUE);
    return s;
}

static void build_window(GtkApplication *app)
{
    A.win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(A.win), "Nautilus LCD");
    gtk_window_set_default_size(GTK_WINDOW(A.win), 440, 860);

    GtkWidget *hb = gtk_header_bar_new();
    GtkWidget *title = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    A.dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(A.dot, "dot");
    gtk_widget_set_valign(A.dot, GTK_ALIGN_CENTER);
    GtkWidget *tl = gtk_label_new("Nautilus LCD");
    gtk_widget_add_css_class(tl, "title");
    gtk_box_append(GTK_BOX(title), A.dot);
    gtk_box_append(GTK_BOX(title), tl);
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hb), title);
    gtk_window_set_titlebar(GTK_WINDOW(A.win), hb);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_window_set_child(GTK_WINDOW(A.win), scroll);

    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(main, 20);
    gtk_widget_set_margin_end(main, 20);
    gtk_widget_set_margin_top(main, 12);
    gtk_widget_set_margin_bottom(main, 20);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), main);

    /* round preview */
    GtkWidget *lcd = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(lcd, "lcd");
    gtk_widget_set_overflow(lcd, GTK_OVERFLOW_HIDDEN);
    gtk_widget_set_size_request(lcd, PREVIEW_SIZE, PREVIEW_SIZE);
    gtk_widget_set_halign(lcd, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(lcd, 16);
    A.preview = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(A.preview), GTK_CONTENT_FIT_COVER);
    gtk_widget_set_hexpand(A.preview, TRUE);
    gtk_widget_set_vexpand(A.preview, TRUE);
    gtk_box_append(GTK_BOX(lcd), A.preview);
    gtk_box_append(GTK_BOX(main), lcd);

    A.status = gtk_label_new("Connecting to the service…");
    gtk_widget_add_css_class(A.status, "status");
    gtk_label_set_ellipsize(GTK_LABEL(A.status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_top(A.status, 12);
    gtk_box_append(GTK_BOX(main), A.status);

    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(actions, GTK_ALIGN_CENTER);
    GtkWidget *stop = pill("Stop", NULL);
    GtkWidget *hw = pill("Default screen", NULL);
    A.start_btn = pill("Start service", "accent");
    gtk_widget_set_visible(A.start_btn, FALSE);
    g_signal_connect(stop, "clicked", G_CALLBACK(on_stop), NULL);
    g_signal_connect(hw, "clicked", G_CALLBACK(on_hardware), NULL);
    g_signal_connect(A.start_btn, "clicked", G_CALLBACK(on_start_service), NULL);
    GtkColorDialog *sdlg = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(sdlg, FALSE);
    gtk_color_dialog_set_title(sdlg, "Solid color");
    A.cpu_bg = gtk_color_dialog_button_new(sdlg);
    gtk_widget_add_css_class(A.cpu_bg, "swatch-btn");
    gtk_widget_set_tooltip_text(A.cpu_bg, "Solid color background with the CPU temperature");
    GdkRGBA black = { 0, 0, 0, 1 };
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(A.cpu_bg), &black);
    g_signal_connect(A.cpu_bg, "notify::rgba", G_CALLBACK(on_solid_color), NULL);
    gtk_box_append(GTK_BOX(actions), stop);
    gtk_box_append(GTK_BOX(actions), hw);
    gtk_box_append(GTK_BOX(actions), A.cpu_bg);
    gtk_box_append(GTK_BOX(actions), A.start_btn);
    gtk_box_append(GTK_BOX(main), actions);

    /* library */
    GtkWidget *lib = card();
    GtkWidget *lh = row();
    GtkWidget *add = pill("+ Add", "accent");
    gtk_widget_add_css_class(add, "small");
    GtkWidget *rm = pill("Remove", "danger");
    gtk_widget_add_css_class(rm, "small");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add), NULL);
    g_signal_connect(rm, "clicked", G_CALLBACK(on_remove), NULL);
    GtkWidget *ll = label_new("LIBRARY", "section");
    gtk_widget_set_valign(ll, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(lh), ll);
    gtk_box_append(GTK_BOX(lh), spacer());
    gtk_box_append(GTK_BOX(lh), rm);
    gtk_box_append(GTK_BOX(lh), add);
    gtk_box_append(GTK_BOX(lib), lh);

    A.flow = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(A.flow), GTK_SELECTION_SINGLE);
    gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(A.flow), TRUE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(A.flow), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(A.flow), 4);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(A.flow), 5);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(A.flow), 4);
    g_signal_connect(A.flow, "child-activated", G_CALLBACK(on_thumb_activated), NULL);
    gtk_box_append(GTK_BOX(lib), A.flow);

    A.empty = label_new("Add images or GIFs — click a thumbnail to show it.", "muted");
    gtk_label_set_wrap(GTK_LABEL(A.empty), TRUE);
    gtk_box_append(GTK_BOX(lib), A.empty);
    gtk_box_append(GTK_BOX(main), lib);

    /* CPU temperature */
    GtkWidget *tc = card();
    GtkWidget *th = row();
    GtkWidget *tl2 = label_new("CPU TEMPERATURE", "section");
    gtk_widget_set_valign(tl2, GTK_ALIGN_CENTER);
    A.cpu_switch = gtk_switch_new();
    gtk_widget_set_valign(A.cpu_switch, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(A.cpu_switch, "Show the temperature over the selected image");
    g_signal_connect(A.cpu_switch, "notify::active", G_CALLBACK(on_cpu_switch), NULL);
    gtk_box_append(GTK_BOX(th), tl2);
    gtk_box_append(GTK_BOX(th), spacer());
    gtk_box_append(GTK_BOX(th), A.cpu_switch);
    gtk_box_append(GTK_BOX(tc), th);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    GtkFontDialog *fdlg = gtk_font_dialog_new();
    gtk_font_dialog_set_title(fdlg, "Font");
    A.cpu_font = gtk_font_dialog_button_new(fdlg);
    gtk_font_dialog_button_set_level(GTK_FONT_DIALOG_BUTTON(A.cpu_font), GTK_FONT_LEVEL_FACE);
    gtk_font_dialog_button_set_use_font(GTK_FONT_DIALOG_BUTTON(A.cpu_font), TRUE);
    set_font_button("Sans Bold");

    GtkColorDialog *cdlg = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(cdlg, FALSE);
    A.cpu_fg = gtk_color_dialog_button_new(cdlg);
    GdkRGBA white = { 1, 1, 1, 1 };
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(A.cpu_fg), &white);

    A.cpu_label = gtk_entry_new();
    gtk_widget_add_css_class(A.cpu_label, "dark");
    gtk_entry_set_max_length(GTK_ENTRY(A.cpu_label), 24);
    gtk_entry_set_placeholder_text(GTK_ENTRY(A.cpu_label), "optional, e.g. CPU");
    gtk_editable_set_text(GTK_EDITABLE(A.cpu_label), "CPU");


    struct { const char *name; GtkWidget *w; } fields[] = {
        { "Font", A.cpu_font }, { "Text color", A.cpu_fg }, { "Label", A.cpu_label },
    };
    for (int i = 0; i < 3; i++) {
        GtkWidget *l = label_new(fields[i].name, "field");
        gtk_widget_set_valign(l, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(fields[i].w, "picker");
        gtk_widget_set_hexpand(fields[i].w, TRUE);
        if (i == 1)
            gtk_widget_set_halign(fields[i].w, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), fields[i].w, 1, i, 1, 1);
    }
    gtk_box_append(GTK_BOX(tc), grid);
    g_signal_connect(A.cpu_font, "notify::font-desc", G_CALLBACK(on_cpu_notify), NULL);
    g_signal_connect(A.cpu_fg, "notify::rgba", G_CALLBACK(on_cpu_notify), NULL);
    g_signal_connect(A.cpu_label, "changed", G_CALLBACK(on_cpu_label_changed), NULL);
    gtk_box_append(GTK_BOX(main), tc);

    /* settings */
    GtkWidget *sc = card();
    GtkWidget *br = row();
    gtk_box_append(GTK_BOX(br), label_new("BRIGHTNESS", "section"));
    gtk_box_append(GTK_BOX(br), spacer());
    A.bright_val = label_new("100%", "value");
    gtk_label_set_xalign(GTK_LABEL(A.bright_val), 1);
    gtk_box_append(GTK_BOX(br), A.bright_val);
    gtk_box_append(GTK_BOX(sc), br);

    A.bright = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
    gtk_scale_set_draw_value(GTK_SCALE(A.bright), FALSE);
    A.applying = TRUE;
    gtk_range_set_value(GTK_RANGE(A.bright), 100);
    A.applying = FALSE;
    g_signal_connect(A.bright, "value-changed", G_CALLBACK(on_brightness), NULL);
    gtk_box_append(GTK_BOX(sc), A.bright);

    GtkWidget *rl = label_new("ROTATION", "section");
    gtk_widget_set_margin_top(rl, 6);
    gtk_box_append(GTK_BOX(sc), rl);
    GtkWidget *rr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(rr), TRUE);
    static const char *rot_labels[4] = { "0°", "90°", "180°", "270°" };
    for (int i = 0; i < 4; i++) {
        A.rot[i] = gtk_toggle_button_new_with_label(rot_labels[i]);
        gtk_widget_add_css_class(A.rot[i], "seg");
        if (i > 0)
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(A.rot[i]), GTK_TOGGLE_BUTTON(A.rot[0]));
        g_signal_connect(A.rot[i], "toggled", G_CALLBACK(on_rotation), GINT_TO_POINTER(i));
        gtk_box_append(GTK_BOX(rr), A.rot[i]);
    }
    gtk_box_append(GTK_BOX(sc), rr);
    gtk_box_append(GTK_BOX(main), sc);
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

static void on_activate(GtkApplication *app, gpointer u)
{
    (void)u;
    if (A.win) {
        gtk_window_present(GTK_WINDOW(A.win));
        return;
    }
    g_object_set(gtk_settings_get_default(),
                 "gtk-interface-color-scheme", GTK_INTERFACE_COLOR_SCHEME_DARK, NULL);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css, CSS);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    build_window(app);
    library_refresh();
    gtk_window_present(GTK_WINDOW(A.win));

    poll(NULL);
    g_timeout_add(POLL_MS, poll, NULL);
}

int main(int argc, char **argv)
{
    A.textures = g_ptr_array_new_with_free_func(g_object_unref);
    A.delays = g_array_new(FALSE, FALSE, sizeof(int));
    A.thumbs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    A.preview_key = g_strdup("");
    A.lib_dir = g_build_filename(g_get_user_data_dir(), "nautilusctl", "library", NULL);
    g_mkdir_with_parents(A.lib_dir, 0755);

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    int rc = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);

    g_ptr_array_unref(A.textures);
    g_array_unref(A.delays);
    g_hash_table_unref(A.thumbs);
    g_free(A.preview_key);
    g_free(A.lib_dir);
    g_free(A.flash);
    g_free(A.cur_image);
    return rc;
}
