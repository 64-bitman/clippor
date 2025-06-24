#pragma once

#include <gio/gio.h>

#define STRING(s) _STRING(s)
#define _STRING(s) #s

extern GSettings *SETTINGS;

extern GPtrArray *ALLOWED_MIME_TYPES;
extern GHashTable *MIME_TYPE_GROUPS;

extern GPtrArray *CLIPBOARDS;
extern GPtrArray *CONNECTIONS;
extern GPtrArray *CLIENTS;

extern GMainContext *MAIN_CONTEXT;
extern GMainLoop *MAIN_LOOP;

extern GDBusConnection *DBUS_SERVICE_CONNECTION;
extern uint DBUS_SERVICE_IDENTIFIER;
extern GDBusObjectManagerServer *DBUS_SERVICE_OBJ_MANAGER;
