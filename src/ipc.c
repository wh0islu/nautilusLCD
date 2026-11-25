/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * ipc.c — client for the service's Unix socket, using GIO.
 */
#include "ipc.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>

#define IPC_TIMEOUT_S 60   /* SHOW of a large GIF can take a few seconds */

char *ipc_socket_path(void)
{
    return g_build_filename(g_get_user_runtime_dir(), "nautilusctl.sock", NULL);
}

char *ipc_request(const char *command, GError **err)
{
    char *path = ipc_socket_path();
    GSocketAddress *addr = g_unix_socket_address_new(path);
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, IPC_TIMEOUT_S);
    g_free(path);

    char *reply = NULL;
    GSocketConnection *conn =
        g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, err);
    if (conn) {
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        char *line = g_strconcat(command, "\n", NULL);
        if (g_output_stream_write_all(out, line, strlen(line), NULL, NULL, err)) {
            GDataInputStream *in =
                g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(conn)));
            reply = g_data_input_stream_read_line_utf8(in, NULL, NULL, err);
            if (!reply && err && !*err)
                g_set_error(err, G_IO_ERROR, G_IO_ERROR_CLOSED, "the service closed the connection");
            g_object_unref(in);
        }
        g_free(line);
        g_object_unref(conn);
    }
    g_object_unref(client);
    g_object_unref(addr);
    return reply;
}

GHashTable *ipc_parse_status(const char *reply)
{
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char **parts = g_strsplit(reply ? reply : "", "\t", -1);
    for (int i = 1; parts[i]; i++) {          /* parts[0] is "OK" */
        char *eq = strchr(parts[i], '=');
        if (eq)
            g_hash_table_insert(t, g_strndup(parts[i], eq - parts[i]), g_strdup(eq + 1));
    }
    g_strfreev(parts);
    return t;
}
