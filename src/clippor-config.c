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

        ClipporClipboard *cb = clippor_clipboard_new(label.u.str.ptr);

        if (max_entries.type != TOML_UNKNOWN)
            g_object_set(cb, "max-entries", max_entries.u.int64, NULL);

        g_ptr_array_add(self->clipboards, cb);
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

        WaylandConnection *ct = WAYLAND_FUNCS.connection_new(display.u.str.ptr);

        if (!WAYLAND_FUNCS.connection_start(ct, NULL))
        {
            g_debug(
                "Wayland display '%s' failed to start, ignoring",
                display.u.str.ptr
            );
            g_object_unref(ct);
            continue;
        }

        g_ptr_array_add(self->wayland_connections, ct);

        // Connect seat(s) to clipboards
        if (seats.type == TOML_UNKNOWN)
            continue;

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

            g_autoptr(WaylandSeat) seat_obj =
                WAYLAND_FUNCS.connection_get_seat(ct, name.u.str.ptr);

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

                for (uint r = 0; r < self->clipboards->len; r++)
                {
                    ClipporClipboard *cb = self->clipboards->pdata[r];
                    if (g_strcmp0(
                            clippor_clipboard_get_label(cb), cb_label.u.str.ptr
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

                for (uint r = 0; r < self->clipboards->len; r++)
                {
                    ClipporClipboard *cb = self->clipboards->pdata[r];
                    if (g_strcmp0(
                            clippor_clipboard_get_label(cb), cb_label.u.str.ptr
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
