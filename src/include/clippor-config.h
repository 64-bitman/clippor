#pragma once

#include <glib.h>

typedef struct
{
    // Don't use a hash table since clipboard labels can be changed by the user
    // when the program is running.
    GPtrArray *clipboards;

    GPtrArray *wayland_connections;
    GHashTable *wayland_seat_map; // Each key is a clipboard label and the value
                                  // is a ptr array of seat names.
} ClipporConfig;

typedef enum
{
    CLIPPOR_CONFIG_ERROR_NO_FILE,
    CLIPPOR_CONFIG_ERROR_PARSE,
    CLIPPOR_CONFIG_ERROR_INVALID
} ClipporConfigError;

#define CONFIG_ERROR (config_error_quark())
GQuark config_error_quark(void);

ClipporConfig *clippor_config_new_file(const char *config_file, GError **error);

ClipporConfig *clippor_config_ref(ClipporConfig *cfg);
void clippor_config_unref(ClipporConfig *cfg);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClipporConfig, clippor_config_unref)
