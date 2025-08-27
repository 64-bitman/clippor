#include "clippor-config.h"
#include "clippor-clipboard.h"
#include "modules.h"
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

    g_clear_pointer(&self->clipboards, g_ptr_array_unref);
    g_clear_pointer(&self->wayland_connections, g_ptr_array_unref);
    g_clear_pointer(&self->wayland_seat_map, g_hash_table_unref);
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
                "Array 'allowed_mime_types' in 'clipboards' is not an array"
            );
        if (mime_type_groups.type != TOML_UNKNOWN &&
            mime_type_groups.type != TOML_ARRAY)
            TOML_ERROR(
                "Array 'mime_type_groups' in 'clipboards' is not an array"
            );

        g_autoptr(ClipporClipboard) cb = clippor_clipboard_new(label.u.str.ptr);

        if (max_entries.type != TOML_UNKNOWN)
            g_object_set(cb, "max-entries", max_entries.u.int64, NULL);

        if (allowed_mime_types.type == TOML_ARRAY)
        {
            g_autoptr(GError) error = NULL;
            g_autoptr(GPtrArray) arr =
                g_ptr_array_new_with_free_func((GDestroyNotify)g_regex_unref);

            for (int k = 0; k < allowed_mime_types.u.arr.size; k++)
            {
                toml_datum_t entry = allowed_mime_types.u.arr.elem[i];

                if (entry.type != TOML_STRING)
                    TOML_ERROR(
                        "allowed_mime_types in 'clipboards' must only contain "
                        "strings"
                    );

                GRegex *regex = g_regex_new(entry.u.str.ptr, G_REGEX_OPTIMIZE,
                        G_REGEX_MATCH_DEFAULT, &error);

                if (regex == NULL)
                    TOML_ERROR(error->message);

                g_ptr_array_add(arr, regex);
            }

            g_object_set(cb, "allowed-mime-types", arr, NULL);
        }

        g_ptr_array_add(self->clipboards, g_object_ref(cb));
    }

skip_clipboards:;
    // Parse configuration for Wayland
    toml_datum_t wayland_displays =
        toml_seek(result.toptab, "wayland_displays");

    if (!WAYLAND_FUNCS.available || wayland_displays.type == TOML_UNKNOWN)
        goto skip_wayland;

    if (wayland_displays.type != TOML_ARRAY)
        TOML_ERROR("Array 'wayland_displays' is not an array");

    for (int i = 0; i < wayland_displays.u.arr.size; i++)
    {
        toml_datum_t wayland_display = wayland_displays.u.arr.elem[i];

        toml_datum_t display = toml_seek(wayland_display, "display");
        toml_datum_t seats = toml_seek(wayland_display, "seats");

        if (display.type != TOML_STRING)
            TOML_ERROR(
                "Option 'display' in 'wayland_displays' is not a string or "
                "does not exist"
            );
        if (seats.type != TOML_UNKNOWN && seats.type != TOML_ARRAY)
            TOML_ERROR("Array 'seats' in 'wayland_displays' is not an array");

        const char *actual_display = display.u.str.ptr;

        // Expand env var
        if (actual_display[0] == '$')
            actual_display = g_getenv(actual_display + 1);

        if (actual_display == NULL)
            continue;

        g_autoptr(WaylandConnection) ct =
            WAYLAND_FUNCS.connection_new(actual_display);

        if (!WAYLAND_FUNCS.connection_start(ct, NULL))
        {
            g_debug(
                "Wayland display '%s' failed to start, ignoring",
                display.u.str.ptr
            );
            continue;
        }

        g_ptr_array_add(self->wayland_connections, g_object_ref(ct));

        if (seats.type == TOML_UNKNOWN)
            continue;

        // Connect seat(s) to clipboards
        for (int k = 0; k < seats.u.arr.size; k++)
        {
            toml_datum_t seat = seats.u.arr.elem[k];

            if (seat.type != TOML_TABLE)
                TOML_ERROR(
                    "Table 'seats' in 'wayland_displays' should only contain "
                    "tables"
                );

            toml_datum_t name = toml_seek(seat, "seat");
            toml_datum_t regular = toml_seek(seat, "regular");
            toml_datum_t primary = toml_seek(seat, "primary");

            if (name.type != TOML_STRING)
                TOML_ERROR(
                    "Option 'seat' in 'seats' is not a string or does not exist"
                );
            if (regular.type != TOML_UNKNOWN && regular.type != TOML_TABLE)
                TOML_ERROR("Table 'regular' in 'seat' is not a table");
            if (primary.type != TOML_UNKNOWN && primary.type != TOML_TABLE)
                TOML_ERROR("Table 'primary' in 'seat' is not a table");

            const char *actual_seat = name.u.str.ptr;

            // Expand env var
            if (actual_seat[0] == '$')
                actual_seat = g_getenv(actual_seat + 1);

            if (actual_seat == NULL)
                continue;

            g_autoptr(WaylandSeat) seat_obj =
                WAYLAND_FUNCS.connection_get_seat(ct, actual_seat);

            if (regular.type != TOML_UNKNOWN)
            {
                toml_datum_t cb_label = toml_seek(regular, "clipboard");

                if (cb_label.type != TOML_STRING)
                    TOML_ERROR(
                        "Option 'clipboard' in 'regular' is not a string or "
                        "does not exist"
                    );

                WaylandSelection *sel = WAYLAND_FUNCS.seat_get_selection(
                    seat_obj, CLIPPOR_SELECTION_TYPE_REGULAR
                );

                if (sel != NULL)
                {
                    for (uint r = 0; r < self->clipboards->len; r++)
                    {
                        ClipporClipboard *cb = self->clipboards->pdata[r];
                        if (g_strcmp0(
                                clippor_clipboard_get_label(cb),
                                cb_label.u.str.ptr
                            ) == 0)
                        {
                            clippor_clipboard_connect_selection(
                                cb, CLIPPOR_SELECTION(sel)
                            );
                            break;
                        }
                    }
                    g_object_unref(sel);
                }
            }

            if (primary.type != TOML_UNKNOWN)
            {
                toml_datum_t cb_label = toml_seek(primary, "clipboard");

                if (cb_label.type != TOML_STRING)
                    TOML_ERROR(
                        "Option 'clipboard' in 'primary' is not a string or "
                        "does not exist"
                    );

                WaylandSelection *sel = WAYLAND_FUNCS.seat_get_selection(
                    seat_obj, CLIPPOR_SELECTION_TYPE_PRIMARY
                );

                if (sel != NULL)
                {
                    for (uint r = 0; r < self->clipboards->len; r++)
                    {
                        ClipporClipboard *cb = self->clipboards->pdata[r];
                        if (g_strcmp0(
                                clippor_clipboard_get_label(cb),
                                cb_label.u.str.ptr
                            ) == 0)
                        {
                            clippor_clipboard_connect_selection(
                                cb, CLIPPOR_SELECTION(sel)
                            );
                            break;
                        }
                    }

                    g_object_unref(sel);
                }
            }
        }
    }
skip_wayland:

    toml_free(result);
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
    ClipporConfig *cfg = g_rc_box_new(ClipporConfig);

    cfg->clipboards = g_ptr_array_new_with_free_func(g_object_unref);
    cfg->wayland_connections = g_ptr_array_new_with_free_func(g_object_unref);
    cfg->wayland_seat_map = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref
    );

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
        clippor_config_unref(cfg);
        return NULL;
    }

    return cfg;
}

ClipporConfig *
clippor_config_ref(ClipporConfig *self)
{
    g_assert(self != NULL);

    g_rc_box_acquire(self);

    return self;
}

void
clippor_config_unref(ClipporConfig *self)
{
    g_assert(self != NULL);

    g_rc_box_release_full(self, (GDestroyNotify)clippor_config_free);
}
