#include "server.h"
#include "clippor-client.h"
#include "config.h"
#include "database.h"
#include "dbus-service.h"
#include "glib-object.h"
#include "glib-unix.h"
#include "wayland-connection.h"
#include "wayland-seat.h"
#include <glib.h>

G_DEFINE_QUARK(server_error_quark, server_error)

static GMainLoop *MAIN_LOOP;

// Each key is the name/label of the object and its value is the object
static GHashTable *CLIPBOARDS;
static GHashTable *WAYLAND_CONNECTIONS;

static Config *CONFIG;

static uint SIGNAL_SOURCE_TAGS[2];

static void
wayland_connections_free_func(GWeakRef *ref)
{
    WaylandConnection *ct = g_weak_ref_get(ref);

    if (ct != NULL)
    {
        // Unref twice because g_weak_ref_get creates a strong ref too
        g_object_unref(ct);
        g_object_unref(ct);
    }

    g_weak_ref_clear(ref);
    g_free(ref);
}

void
server_free(uint flags)
{
    if (MAIN_LOOP != NULL)
        g_clear_pointer(&MAIN_LOOP, g_main_loop_unref);

    wayland_connection_free_static();
    g_hash_table_unref(WAYLAND_CONNECTIONS);
    g_hash_table_unref(CLIPBOARDS);

    config_free(CONFIG);
    dbus_server_uninit();

    if (!(flags & SERVER_FLAG_NO_UNINIT_DB))
        database_uninit();
}

static gboolean
on_exit_signal(gpointer user_data G_GNUC_UNUSED)
{
    for (uint i = 0; i < G_N_ELEMENTS(SIGNAL_SOURCE_TAGS); i++)
        g_source_remove(SIGNAL_SOURCE_TAGS[i]);

    g_main_loop_quit(MAIN_LOOP);

    g_info("Exiting...");

    return G_SOURCE_REMOVE;
}

static void
server_set_config_add_wayland_seat(
    ConfigWaylandDisplay *config_dpy, ConfigWaylandSeat *config_seat,
    WaylandSeat *seat
)
{
    ClipporClipboard *cb =
        g_hash_table_lookup(CLIPBOARDS, config_seat->clipboard);

    if (cb == NULL)
        return;

    g_autofree char *label = g_strdup_printf(
        "%s_%s", config_dpy->display, wayland_seat_get_name(seat)
    );

    if (config_seat->regular)
        clippor_clipboard_add_client(
            cb, label, CLIPPOR_CLIENT(seat), CLIPPOR_SELECTION_TYPE_REGULAR
        );
    if (config_seat->primary)
        clippor_clipboard_add_client(
            cb, label, CLIPPOR_CLIENT(seat), CLIPPOR_SELECTION_TYPE_PRIMARY
        );
}

static void
server_set_config(void)
{
    // Add clipboards
    for (uint i = 0; i < CONFIG->clipboards->len; i++)
    {
        ConfigClipboard config_cb =
            g_array_index(CONFIG->clipboards, ConfigClipboard, i);

        if (g_hash_table_contains(CLIPBOARDS, config_cb.name))
        {
            g_warning("Clipboard '%s' already exists", config_cb.name);
            continue;
        }

        ClipporClipboard *cb = clippor_clipboard_new(config_cb.name);

        if (cb == NULL)
            continue;

        g_object_set(
            cb, "max-entries", config_cb.max_entries, "max-entries-memory",
            config_cb.max_entries_memory, "allowed-mime-types",
            config_cb.allowed_mime_types, "mime-type-groups",
            config_cb.mime_type_groups, NULL
        );

        g_hash_table_insert(CLIPBOARDS, g_strdup(config_cb.name), cb);
    }

    // Get Wayland displays and add seats to their respective clipboard
    for (uint i = 0; i < CONFIG->wayland_displays->len; i++)
    {
        ConfigWaylandDisplay config_dpy =
            g_array_index(CONFIG->wayland_displays, ConfigWaylandDisplay, i);

        if (g_hash_table_contains(WAYLAND_CONNECTIONS, config_dpy.display))
        {
            g_warning(
                "Wayland connection '%s' already exists", config_dpy.display
            );
            continue;
        }

        GError *error = NULL;
        WaylandConnection *ct =
            wayland_connection_new(config_dpy.display, &error);

        if (ct == NULL)
        {
            g_warning("%s", error->message);
            g_error_free(error);
            continue;
        }

        GWeakRef *ref = g_new(GWeakRef, 1);

        g_weak_ref_init(ref, ct);

        g_hash_table_insert(
            WAYLAND_CONNECTIONS, g_strdup(config_dpy.display), ref
        );

        for (uint k = 0; k < config_dpy.seats->len; k++)
        {
            ConfigWaylandSeat config_seat =
                g_array_index(config_dpy.seats, ConfigWaylandSeat, k);

            if (config_seat.name == NULL)
            {
                g_assert(config_dpy.seats->len == 1);
                // If NULL add all seats
                GHashTableIter iter;
                WaylandSeat *seat;

                g_hash_table_iter_init(&iter, wayland_connection_get_seats(ct));

                while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&seat))
                {
                    server_set_config_add_wayland_seat(
                        &config_dpy, &config_seat, seat
                    );
                }
                break;
            }

            WaylandSeat *seat =
                wayland_connection_match_seat(ct, config_seat.name);

            if (seat == NULL)
                continue;

            server_set_config_add_wayland_seat(&config_dpy, &config_seat, seat);
        }

        wayland_connection_install_source(
            ct, g_main_context_get_thread_default()
        );
    }
}

gboolean
server_start(const char *config_value, const char *data_directory, uint flags)
{
    GError *error = NULL;

    gboolean config_is_file = flags & SERVER_FLAG_USE_CONFIG_FILE;
    gboolean no_db = flags & SERVER_FLAG_NO_INIT_DB;
    gboolean db_in_memory = flags & SERVER_FLAG_DB_IN_MEMORY;

    if ((CONFIG = config_init(config_value, config_is_file, &error)) == NULL)
        goto fail;
    if (!no_db && !database_init(data_directory, db_in_memory, &error))
        goto fail;
    if (CONFIG->dbus_service &&
        !dbus_service_init(CONFIG->dbus_timeout, &error))
        goto fail;

    CLIPBOARDS =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    WAYLAND_CONNECTIONS = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free,
        (GDestroyNotify)wayland_connections_free_func
    );

    server_set_config();

    if (!(flags & SERVER_FLAG_MANUAL))
    {
        MAIN_LOOP = g_main_loop_new(g_main_context_get_thread_default(), FALSE);
        SIGNAL_SOURCE_TAGS[0] = g_unix_signal_add(SIGINT, on_exit_signal, NULL);
        SIGNAL_SOURCE_TAGS[1] =
            g_unix_signal_add(SIGTERM, on_exit_signal, NULL);

        g_main_loop_run(MAIN_LOOP);

        server_free(SERVER_FLAG_NONE);
    }

    return TRUE;
fail:
    g_assert(error != NULL);

    g_warning("Failed starting server: %s", error->message);
    g_error_free(error);

    return FALSE;
}

GMainLoop *
server_get_main_loop(void)
{
    return MAIN_LOOP;
}

/*
 * Used by tests
 */
ClipporClipboard *
server_get_clipboard(const char *label)
{
    g_assert(label != NULL);
    return g_hash_table_lookup(CLIPBOARDS, label);
}

GHashTable *
server_get_clipboards(void)
{
    return CLIPBOARDS;
}

/*
 * Creates a new reference to "cb" on success
 */
gboolean
server_add_clipboard(ClipporClipboard *cb, GError **error)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(error == NULL || *error == NULL);

    const char *label = clippor_clipboard_get_label(cb);

    if (g_hash_table_contains(CLIPBOARDS, label))
    {
        g_set_error(
            error, SERVER_ERROR, SERVER_ERROR_CLIPBOARD_EXISTS,
            "Clipboard already exists in server"
        );
        return FALSE;
    }

    g_hash_table_insert(CLIPBOARDS, g_strdup(label), g_object_ref(cb));

    return TRUE;
}

void
server_remove_clipboard(const char *label)
{
    g_assert(label != NULL);
    g_hash_table_remove(CLIPBOARDS, label);
}

GHashTable *
server_get_wayland_connections(void)
{
    return WAYLAND_CONNECTIONS;
}

/*
 * Does not create a new reference to "ct" on success
 */
gboolean
server_add_wayland_connection(WaylandConnection *ct, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(ct));
    g_assert(error == NULL || *error == NULL);

    const char *display = wayland_connection_get_display_name(ct);

    if (g_hash_table_contains(WAYLAND_CONNECTIONS, display))
    {
        g_set_error(
            error, SERVER_ERROR, SERVER_ERROR_WAYLAND_CONNECTION_EXISTS,
            "Wayland connection already exists in server"
        );
        return FALSE;
    }

    GWeakRef *ref = g_new(GWeakRef, 1);

    g_weak_ref_init(ref, ct);
    g_hash_table_insert(WAYLAND_CONNECTIONS, g_strdup(display), ref);

    return TRUE;
}

ClipporClipboard *
server_get_wayland_connection(const char *display)
{
    g_assert(display != NULL);
    return g_hash_table_lookup(WAYLAND_CONNECTIONS, display);
}
