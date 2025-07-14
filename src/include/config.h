#pragma once

#include <glib.h>

typedef struct
{
    char *name;

    int64_t max_entries;
    int64_t max_entries_memory;

    GPtrArray *allowed_mime_types;
    GHashTable *mime_type_groups;
} ConfigClipboard;

typedef struct
{
    GRegex *name;
    char *clipboard;

    gboolean regular;
    gboolean primary;
} ConfigWaylandSeat;

typedef struct
{
    char *display;

    int connection_timeout;
    int data_timeout;

    GArray *seats; // May contain one seat with a NULL name to assume all
                   // seats.
} ConfigWaylandDisplay;

typedef struct
{
    gboolean dbus_service;
    int dbus_timeout;

    GArray *clipboards;
    GArray *wayland_displays;
} Config;

#define CONFIG_ERROR (config_error_quark())

typedef enum
{
    CONFIG_ERROR_NO_FILE,
    CONFIG_ERROR_PARSE,
    CONFIG_ERROR_INVALID,
} ConfigError;

GQuark config_error_quark(void);

Config *config_init(const char *user_config, gboolean file, GError **error);
void config_free(Config *config);
