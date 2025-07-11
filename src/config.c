#include "config.h"
#include "tomlc17.h"
#include "util.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

G_DEFINE_QUARK(config_error_quark, config_error)

void
config_clipboard_free(ConfigClipboard *config_cb)
{
    g_assert(config_cb != NULL);

    g_free(config_cb->name);
    g_ptr_array_unref(config_cb->allowed_mime_types);
    g_hash_table_unref(config_cb->mime_type_groups);
}

void
config_wayland_seat_free(ConfigWaylandSeat *config_seat)
{
    g_assert(config_seat != NULL);

    if (config_seat->name != NULL)
        g_regex_unref(config_seat->name);
    g_free(config_seat->clipboard);
}

void
config_wayland_display_free(ConfigWaylandDisplay *config_dpy)
{
    g_assert(config_dpy != NULL);

    g_free(config_dpy->display);
    g_array_unref(config_dpy->seats);
}

#define TOML_ERROR(msg)                                                        \
    do                                                                         \
    {                                                                          \
        g_set_error(                                                           \
            error, CONFIG_ERROR, CONFIG_ERROR_INVALID,                         \
            "Failed parsing configuration file: %s", msg                       \
        );                                                                     \
        goto fail;                                                             \
    } while (0)

#define TOML_IS_NOT_TYPE(s, t) (s.type != TOML_UNKNOWN && s.type != t)
#define TOML_IS_NOT_TYPE2(s, t) (s.type != t)

#define TOML_SET(s, w, v, d)                                                   \
    do                                                                         \
    {                                                                          \
        if (s.type == TOML_UNKNOWN)                                            \
            v = d;                                                             \
        else                                                                   \
            v = s.w;                                                           \
    } while (0)

/*
 * Populate config with values from configuration file.
 */
static gboolean
config_populate(Config *config, const char *config_file, GError **error)
{
    g_assert(config != NULL);
    g_assert(error == NULL || *error == NULL);

    toml_result_t result = toml_parse_file_ex(config_file);

    if (!result.ok)
        TOML_ERROR(result.errmsg);

    toml_datum_t dbus_timeout = toml_seek(result.toptab, "dbus_timeout");
    toml_datum_t dbus_service = toml_seek(result.toptab, "dbus_service");

    TOML_SET(dbus_timeout, u.int64, config->dbus_timeout, 500);
    TOML_SET(dbus_service, u.boolean, config->dbus_service, TRUE);

    if (config->dbus_timeout < -1)
        TOML_ERROR("'dbus_timeout' must be greater than or equal to -1");

    toml_datum_t clipboards = toml_seek(result.toptab, "clipboards");

    if (TOML_IS_NOT_TYPE(clipboards, TOML_ARRAY))
        TOML_ERROR("'clipboards' is not a table or does not exist");
    else if (clipboards.type == TOML_UNKNOWN)
        goto skip_clipboards;

    for (int i = 0; i < clipboards.u.arr.size; i++)
    {
        toml_datum_t clipboard = clipboards.u.arr.elem[i];

        if (clipboard.type != TOML_TABLE)
            TOML_ERROR("'clipboards' must only contain tables");

        toml_datum_t clipboard_name = toml_seek(clipboard, "clipboard");
        toml_datum_t max_entries = toml_seek(clipboard, "max_entries");
        toml_datum_t max_entries_memory =
            toml_seek(clipboard, "max_entries_memory");
        toml_datum_t allowed_mime_types =
            toml_seek(clipboard, "allowed_mime_types");
        toml_datum_t mime_type_groups =
            toml_seek(clipboard, "mime_type_groups");

        if (TOML_IS_NOT_TYPE2(clipboard_name, TOML_STRING))
            TOML_ERROR("'clipboard' is not a string or does not exist");
        if (TOML_IS_NOT_TYPE(max_entries, TOML_INT64))
            TOML_ERROR("'max_entries' is not an integer");
        if (TOML_IS_NOT_TYPE(max_entries_memory, TOML_INT64))
            TOML_ERROR("'max_entries_memory' is not an integer");
        if (TOML_IS_NOT_TYPE(allowed_mime_types, TOML_ARRAY))
            TOML_ERROR("'allowed_mime_types' is not an array");
        if (TOML_IS_NOT_TYPE(mime_type_groups, TOML_ARRAY))
            TOML_ERROR("'mime_type_groups' is not an array");

        ConfigClipboard config_cb;

        TOML_SET(max_entries, u.int64, config_cb.max_entries, 100);
        TOML_SET(max_entries_memory, u.int64, config_cb.max_entries_memory, 10);

        if (config_cb.max_entries <= 0)
            TOML_ERROR("'max_entries' must be greater than zero");
        if (config_cb.max_entries_memory <= 0)
            TOML_ERROR("'max_entries_memory' must be greater than zero");

        config_cb.name = g_strdup(clipboard_name.u.str.ptr);
        config_cb.allowed_mime_types =
            g_ptr_array_new_with_free_func((GDestroyNotify)g_regex_unref);

        config_cb.mime_type_groups = g_hash_table_new_full(
            g_direct_hash, g_direct_equal, (GDestroyNotify)g_regex_unref,
            (GDestroyNotify)g_ptr_array_unref
        );

        g_array_append_val(config->clipboards, config_cb);

        if (allowed_mime_types.type != TOML_UNKNOWN)
            for (int k = 0; k < allowed_mime_types.u.arr.size; k++)
            {
                toml_datum_t mime_type_pattern =
                    allowed_mime_types.u.arr.elem[k];

                if (TOML_IS_NOT_TYPE2(mime_type_pattern, TOML_STRING))
                    TOML_ERROR(
                        "'allowed_mime_types' must only contain strings"
                    );

                GRegex *regex = g_regex_new(
                    mime_type_pattern.u.str.ptr, G_REGEX_OPTIMIZE,
                    G_REGEX_MATCH_DEFAULT, error
                );

                if (regex == NULL)
                    goto fail;

                g_ptr_array_add(config_cb.allowed_mime_types, regex);
            }

        if (mime_type_groups.type != TOML_UNKNOWN)
            for (int k = 0; k < mime_type_groups.u.arr.size; k++)
            {
                toml_datum_t mime_type_group = mime_type_groups.u.arr.elem[k];

                if (TOML_IS_NOT_TYPE2(mime_type_group, TOML_TABLE))
                    TOML_ERROR("'mime_type_groups' must only contain tables");

                toml_datum_t mime_type_pattern =
                    toml_seek(mime_type_group, "mime_type");
                toml_datum_t group = toml_seek(mime_type_group, "group");

                if (TOML_IS_NOT_TYPE2(mime_type_pattern, TOML_STRING))
                    TOML_ERROR("'mime_type' is not a string or does not exist");
                if (TOML_IS_NOT_TYPE2(group, TOML_ARRAY))
                    TOML_ERROR("'group' is not an array or does not exist");

                GPtrArray *mime_types = g_ptr_array_new_with_free_func(g_free);

                for (int j = 0; j < group.u.arr.size; j++)
                {
                    toml_datum_t mime_type = group.u.arr.elem[j];

                    if (TOML_IS_NOT_TYPE2(mime_type, TOML_STRING))
                    {
                        g_ptr_array_unref(mime_types);
                        TOML_ERROR("'group' must only contain strings");
                    }

                    g_ptr_array_add(mime_types, g_strdup(mime_type.u.str.ptr));
                }

                GRegex *regex = g_regex_new(
                    mime_type_pattern.u.str.ptr, G_REGEX_OPTIMIZE,
                    G_REGEX_MATCH_DEFAULT, error
                );

                if (regex == NULL)
                {
                    g_ptr_array_unref(mime_types);
                    goto fail;
                }

                g_hash_table_insert(
                    config_cb.mime_type_groups, regex, mime_types
                );
            }
    }
skip_clipboards:;

    toml_datum_t wayland_displays =
        toml_seek(result.toptab, "wayland_displays");

    if (TOML_IS_NOT_TYPE(wayland_displays, TOML_ARRAY))
        TOML_ERROR("'wayland_displays' is not a table");
    else if (wayland_displays.type == TOML_UNKNOWN)
        goto skip_wayland_displays;

    for (int i = 0; i < wayland_displays.u.arr.size; i++)
    {
        toml_datum_t wayland_display = wayland_displays.u.arr.elem[i];

        if (TOML_IS_NOT_TYPE2(wayland_display, TOML_TABLE))
            TOML_ERROR("'wayland_displays' must only contain tables");

        toml_datum_t display = toml_seek(wayland_display, "display");
        toml_datum_t connection_timeout =
            toml_seek(wayland_display, "connection_timeout");
        toml_datum_t data_timeout = toml_seek(wayland_display, "data_timeout");
        toml_datum_t seats = toml_seek(wayland_display, "seats");

        if (TOML_IS_NOT_TYPE2(display, TOML_STRING))
            TOML_ERROR("'display' is not a string or does not exist");

        if (TOML_IS_NOT_TYPE(connection_timeout, TOML_INT64))
            TOML_ERROR("'connection_timeout' is not an integer");
        if (TOML_IS_NOT_TYPE(data_timeout, TOML_INT64))
            TOML_ERROR("'data_timeout' is not an integer");

        if (TOML_IS_NOT_TYPE(seats, TOML_ARRAY) &&
            TOML_IS_NOT_TYPE(seats, TOML_TABLE))
            TOML_ERROR("'seats' is not an array or table");

        ConfigWaylandDisplay config_dpy;

        TOML_SET(
            connection_timeout, u.int64, config_dpy.connection_timeout, 500
        );
        TOML_SET(data_timeout, u.int64, config_dpy.data_timeout, 500);

        if (config_dpy.connection_timeout < -1)
            TOML_ERROR("'connection_timeout' must be greater than equal to -1");
        if (config_dpy.data_timeout < -1)
            TOML_ERROR("'data_timeout' must be greater than equal to -1");

        config_dpy.display = util_expand_env(display.u.str.ptr);
        config_dpy.seats = g_array_new(FALSE, TRUE, sizeof(ConfigWaylandSeat));

        g_array_set_clear_func(
            config_dpy.seats, (GDestroyNotify)config_wayland_seat_free
        );

        g_array_append_val(config->wayland_displays, config_dpy);

        gboolean once = FALSE;
        toml_datum_t seat;

        if (seats.type == TOML_TABLE)
        {
            once = TRUE;
            seat = seats;
            goto once;
        }

        if (seats.type != TOML_UNKNOWN)
            for (int k = 0; k < seats.u.arr.size; k++)
            {
                seat = seats.u.arr.elem[k];
once:
                if (TOML_IS_NOT_TYPE2(seat, TOML_TABLE))
                    TOML_ERROR("'seats' must only contain tables");

                toml_datum_t seat_name = toml_seek(seat, "seat");
                toml_datum_t clipboard = toml_seek(seat, "clipboard");
                toml_datum_t regular = toml_seek(seat, "regular");
                toml_datum_t primary = toml_seek(seat, "primary");

                if (!once && TOML_IS_NOT_TYPE2(seat_name, TOML_STRING))
                    TOML_ERROR("'seat' is not a string or does not exist");
                if (TOML_IS_NOT_TYPE2(clipboard, TOML_STRING))
                    TOML_ERROR("'clipboard' is not a string or does not exist");
                if (TOML_IS_NOT_TYPE(regular, TOML_BOOLEAN))
                    TOML_ERROR("'regular' is not a boolean");
                if (TOML_IS_NOT_TYPE(primary, TOML_BOOLEAN))
                    TOML_ERROR("'primary' is not a boolean");

                ConfigWaylandSeat config_seat;

                if (once)
                    config_seat.name = NULL;
                else
                {
                    config_seat.name = g_regex_new(
                        seat_name.u.str.ptr, G_REGEX_OPTIMIZE,
                        G_REGEX_MATCH_DEFAULT, error
                    );
                    if (config_seat.name == NULL)
                    {
                        g_prefix_error_literal(
                            error, "Failed parsing configuration file: "
                        );
                        goto fail;
                    }
                }
                config_seat.clipboard = g_strdup(clipboard.u.str.ptr);

                TOML_SET(regular, u.boolean, config_seat.regular, TRUE);
                TOML_SET(primary, u.boolean, config_seat.primary, FALSE);

                g_array_append_val(config_dpy.seats, config_seat);
                if (once)
                    break;
            }
    }
skip_wayland_displays:

    toml_free(result);
    return TRUE;
fail:
    g_assert(error == NULL || *error != NULL);

    toml_free(result);
    return FALSE;
}

Config *
config_init(const char *user_config, GError **error)
{
    g_assert(error == NULL || *error == NULL);

    g_autofree char *config_file;

    if (user_config == NULL)
    {
        const char *config_dir = g_get_user_config_dir();

        config_file = g_strdup_printf("%s/clippor/config.toml", config_dir);
    }
    else
        config_file = g_strdup(user_config);

    if (g_access(config_file, R_OK) == -1)
    {
        g_set_error_literal(
            error, CONFIG_ERROR, CONFIG_ERROR_NO_FILE,
            "Config file does not exist"
        );
        return NULL;
    }

    Config *config = g_new(Config, 1);

    config->dbus_timeout = 500;
    config->clipboards = g_array_new(FALSE, TRUE, sizeof(ConfigClipboard));
    config->wayland_displays =
        g_array_new(FALSE, TRUE, sizeof(ConfigWaylandDisplay));

    g_array_set_clear_func(
        config->clipboards, (GDestroyNotify)config_clipboard_free
    );
    g_array_set_clear_func(
        config->wayland_displays, (GDestroyNotify)config_wayland_display_free
    );

    if (!config_populate(config, config_file, error))
    {
        config_free(config);
        return NULL;
    }

    return config;
}

void
config_free(Config *config)
{
    g_assert(config != NULL);

    g_array_unref(config->clipboards);
    g_array_unref(config->wayland_displays);
    g_free(config);
}
