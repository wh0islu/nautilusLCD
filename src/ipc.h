/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 lu
 */
/*
 * ipc.h — talks to the nautilusctld service over a Unix socket.
 *
 * Protocol (text, one line per message):
 *   client  -> "COMMAND arguments\n"
 *   service -> "OK ...\n"  or  "ERR message\n"
 *
 * Commands: PING, STATUS, SHOW <absolute path>, CPU <key=value...>,
 *           BRIGHTNESS <0-100>, ROTATION <0-3>, STOP, HARDWARE, INFO
 *
 * STATUS replies with TAB-separated fields:
 *   OK<TAB>connected=1<TAB>playing=1<TAB>mode=image<TAB>brightness=50
 *     <TAB>rotation=2<TAB>temp=31<TAB>cpu_font=...<TAB>image=/path/file.gif
 */
#ifndef IPC_H
#define IPC_H

#include <glib.h>

/* Socket path: $XDG_RUNTIME_DIR/nautilusctl.sock (allocated). */
char *ipc_socket_path(void);

/*
 * Send a command and return the reply line (without '\n'), or NULL with
 * `err` set if the service cannot be reached.
 */
char *ipc_request(const char *command, GError **err);

/* Split the STATUS reply into a key -> value table. */
GHashTable *ipc_parse_status(const char *reply);

#endif
