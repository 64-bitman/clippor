#include "global.h"
#include <gio/gio.h>
#include <glib.h>

GSettings *SETTINGS = NULL;
GPtrArray *ALLOWED_MIME_TYPES = NULL; // Array of regexes
GHashTable *MIME_TYPE_GROUPS = NULL;  // Hash table of array of regexes

GPtrArray *CLIPBOARDS = NULL;
GPtrArray *CONNECTIONS = NULL;
GPtrArray *CLIENTS = NULL;

GMainContext *MAIN_CONTEXT = NULL;
GMainLoop *MAIN_LOOP = NULL;

GDBusConnection *DBUS_SERVICE_CONNECTION = NULL;
uint DBUS_SERVICE_IDENTIFIER = 0;
GDBusObjectManagerServer *DBUS_SERVICE_OBJ_MANAGER = NULL;
