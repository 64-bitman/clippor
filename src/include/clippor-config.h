#pragma once

#include "tomlc17.h"
#include <glib.h>

typedef struct
{
    GPtrArray *clipboards;

    toml_result_t result;

    GHashTable *modules; // Each key is the name of the module and the value is
                         // the toml_datum_t for its configuration

    grefcount ref_count;
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
