#include "clippor-config.h"
#include "clippor-clipboard.h"
#include "tomlc17.h"
#include <glib.h>

G_DEFINE_QUARK(CONFIG_ERROR, config_error)

#define TOML_ERROR(msg)                                                        \
    do                                                                         \
    {                                                                          \
        err_msg = msg;                                                         \
        goto fail;                                                             \
    } while (FALSE);

static void
clippor_config_free(ClipporConfig *self)
{
    g_assert(self != NULL);

    g_ptr_array_unref(self->clipboards);
    g_hash_table_unref(self->modules);
    toml_free(self->result);

    g_free(self);
}

/*
 * Populate config with values. If "file" is TRUE, then config_value is
 * interpreted as a path to a file, else it is used as a string containing the
 * whole configuration.
 */
static gboolean
clippor_config_populate(
    ClipporConfig *self, const char *config_value, gboolean file, GError **error
)
{
    g_assert(self != NULL);
    g_assert(error == NULL || *error == NULL);

    toml_result_t result;

    if (file)
        result = toml_parse_file_ex(config_value);
    else
        result = toml_parse(config_value, strlen(config_value));

    if (!result.ok)
    {
        g_set_error(
            error, CONFIG_ERROR, CLIPPOR_CONFIG_ERROR_PARSE,
            "Failed parsing configuration: %s", result.errmsg
        );
        toml_free(result);
        return FALSE;
    }

    const char *err_msg;

    // Parse clipboards array
    toml_datum_t clipboards = toml_seek(result.toptab, "clipboards");

    if (clipboards.type == TOML_UNKNOWN)
        //  Doesn't exist in configuration
        goto skip_clipboards;

    if (clipboards.type != TOML_ARRAY)
        TOML_ERROR("Option 'clipboards' is not an array");

    for (int i = 0; i < clipboards.u.arr.size; i++)
    {
        toml_datum_t clipboard = clipboards.u.arr.elem[i];

        if (clipboard.type != TOML_TABLE)
            TOML_ERROR("Option 'clipboards' should only contain tables");

        toml_datum_t label = toml_seek(clipboard, "clipboard");
        toml_datum_t max_entries = toml_seek(clipboard, "max_entries");

        toml_datum_t allowed_mime_types =
            toml_seek(clipboard, "allowed_mime_types");
        toml_datum_t mime_type_groups =
            toml_seek(clipboard, "mime_type_groups");

        // Verify types are correct
        if (label.type != TOML_STRING)
            TOML_ERROR(
                "Option 'label' in 'clipboards' is not a string or does not "
                "exist"
            );
        if (max_entries.type != TOML_UNKNOWN && max_entries.type != TOML_INT64)
            TOML_ERROR("Option 'max_entries' in 'clipboards' is not a number");
        if (allowed_mime_types.type != TOML_UNKNOWN &&
            allowed_mime_types.type != TOML_ARRAY)
            TOML_ERROR(
                "Option 'allowed_mime_types' in 'clipboards' is not an array"
            );
        if (mime_type_groups.type != TOML_UNKNOWN &&
            mime_type_groups.type != TOML_ARRAY)
            TOML_ERROR(
                "Option 'mime_type_groups' in 'clipboards' is not an array"
            );

        ClipporClipboard *cb = clippor_clipboard_new(label.u.str.ptr);

        if (max_entries.type != TOML_UNKNOWN)
            g_object_set(cb, "max-entries", max_entries.u.int64, NULL);

        g_ptr_array_add(self->clipboards, cb);
    }

skip_clipboards:;
    // Parse configuration for each module
    toml_datum_t modules = toml_seek(result.toptab, "modules");

    if (modules.type == TOML_UNKNOWN)
        goto skip_modules;

    if (modules.type != TOML_TABLE)
        TOML_ERROR("Option 'modules' is not a table");

    for (int i = 0; i < modules.u.tab.size; i++)
    {
        const char *name = modules.u.tab.key[i];
        toml_datum_t module = modules.u.tab.value[i];

        g_hash_table_insert(self->modules, g_strdup(name), &module);
    }
skip_modules:

    self->result = result;
    return TRUE;
fail:
    g_set_error(
        error, CONFIG_ERROR, CLIPPOR_CONFIG_ERROR_INVALID,
        "Failed parsing configuration: %s", err_msg
    );

    clippor_config_free(self);
    toml_free(result);
    return FALSE;
}

static ClipporConfig *
clippor_config_init(void)
{
    ClipporConfig *cfg = g_new(ClipporConfig, 1);

    cfg->clipboards = g_ptr_array_new_with_free_func(g_object_unref);
    cfg->modules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    g_ref_count_init(&cfg->ref_count);

    return cfg;
}

/*
 * Parses configuration file. If "config_file" is NULL, then use the default
 * location.
 */
ClipporConfig *
clippor_config_new_file(const char *config_file, GError **error)
{
    g_assert(error == NULL || *error == NULL);

    g_autofree char *path = NULL;

    if (config_file == NULL)
        path =
            g_strdup_printf("%s/clippor/config.toml", g_get_user_config_dir());
    else
        path = g_strdup(config_file);

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
    {
        g_set_error(
            error, CONFIG_ERROR, CLIPPOR_CONFIG_ERROR_NO_FILE,
            "Config file does not exist"
        );
        return NULL;
    }

    ClipporConfig *cfg = clippor_config_init();

    if (!clippor_config_populate(cfg, path, TRUE, error))
    {
        g_prefix_error_literal(error, "Failed parsing configuration file: ");
        return NULL;
    }

    return cfg;
}

ClipporConfig *
clippor_config_ref(ClipporConfig *self)
{
    g_assert(self != NULL);

    g_ref_count_inc(&self->ref_count);

    return self;
}

void
clippor_config_unref(ClipporConfig *self)
{
    g_assert(self != NULL);

    if (g_ref_count_dec(&self->ref_count))
        clippor_config_free(self);
}
