#include "config.h"
#include "tomlc17.h"
#include <glib.h>

G_DEFINE_QUARK(CONFIG_ERROR, config_error)

/*
 * Populate config with values. If "file" is TRUE, then config_value is
 * interpreted as a path to a file, else it is used as a string containing the
 * whole configuration.
 */
static gboolean
config_populate(
    Config *cfg, const char *config_value, gboolean file, GError **error
)
{
    g_assert(cfg != NULL);
    g_assert(error == NULL || *error == NULL);

    toml_result_t result;

    if (file)
        result = toml_parse_file_ex(config_value);
    else
        result = toml_parse(config_value, strlen(config_value));

    if (!result.ok)
    {
        toml_free(result);
        g_set_error(
            error, CONFIG_ERROR, CONFIG_ERROR_PARSE,
            "Failed parsing configuration: %s", result.errmsg
        );
        return FALSE;
    }

    cfg->clipboards = g_ptr_array_new();
    cfg->wayland_connections = g_ptr_array_new();

    toml_free(result);
    return TRUE;
}

/*
 * Parses configuration file. If "config_file" is NULL, then use the default
 * location.
 */
Config *
config_new_file(const char *config_file, GError **error)
{
    g_assert(error == NULL || *error == NULL);

    Config *cfg = g_new(Config, 1);

    g_autofree char *path = NULL;

    if (config_file == NULL)
        path =
            g_strdup_printf("%s/clippor/config.toml", g_get_user_config_dir());
    else
        path = g_strdup(config_file);

    if (!config_populate(cfg, path, TRUE, error))
    {
        g_prefix_error(
            error, "Failed parsing configuration file '%s': ", config_file
        );
        g_free(cfg);
        return NULL;
    }

    return cfg;
}

void
config_free(Config *cfg)
{
    g_assert(cfg != NULL);

    g_ptr_array_unref(cfg->clipboards);
    g_ptr_array_unref(cfg->wayland_connections);

    g_free(cfg);
}
