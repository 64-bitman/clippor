#pragma once

#include "clippor-clipboard.h"
#include "wayland-connection.h"
#include <glib.h>

#define SERVER_ERROR (server_error_quark())

typedef enum
{
    SERVER_ERROR_CLIPBOARD_EXISTS,
    SERVER_ERROR_WAYLAND_CONNECTION_EXISTS
} ServerError;

GQuark server_error_quark(void);

typedef enum
{
    SERVER_FLAG_NONE = 0,
    SERVER_FLAG_USE_CONFIG_FILE = 1 << 0,
    SERVER_FLAG_DB_IN_MEMORY = 1 << 1,
    SERVER_FLAG_NO_INIT_DB = 1 << 2,
    SERVER_FLAG_NO_UNINIT_DB = 1 << 3,
    SERVER_FLAG_MANUAL = 1 << 4,
} ServerFlags;

void server_free(uint flags);
gboolean
server_start(const char *config_value, const char *data_directory, uint flags);

GMainLoop *server_get_main_loop(void);
ClipporClipboard *server_get_clipboard(const char *label);
GHashTable *server_get_clipboards(void);

gboolean server_add_clipboard(ClipporClipboard *cb, GError **error);
void server_remove_clipboard(const char *label);

GHashTable *server_get_wayland_connections(void);
gboolean server_add_wayland_connection(WaylandConnection *ct, GError **error);
ClipporClipboard *server_get_wayland_connection(const char *display);
