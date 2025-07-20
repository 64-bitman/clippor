#pragma once

#include <glib.h>

typedef struct
{
    gboolean dbus_service;
    int dbus_timeout;

    GPtrArray *clipboards;
    GPtrArray *wayland_connections;
} Config;

typedef enum
{
    CONFIG_ERROR_PARSE
} ConfigError;

#define CONFIG_ERROR (config_error_quark())
GQuark config_error_quark(void);

Config *config_new_file(const char *config_file, GError **error);
void config_free(Config *cfg);
