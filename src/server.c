#include "server.h"
#include "clippor-client.h"
#include "config.h"
#include "database.h"
#include "dbus-service.h"
#include "glib-object.h"
#include "glib-unix.h"
#include "wayland-connection.h"
#include <glib.h>

static GMainContext *MAIN_CONTEXT;
static GMainLoop *MAIN_LOOP;

static GPtrArray *CLIPBOARDS;
static GPtrArray *WAYLAND_CONNECTIONS;

static Config *CONFIG;

static void
exit_handler(void)
{
    g_info("Exiting...");

    g_main_loop_quit(MAIN_LOOP);
    g_main_loop_unref(MAIN_LOOP);

    g_ptr_array_unref(CLIPBOARDS);
    g_ptr_array_unref(WAYLAND_CONNECTIONS);

    config_free(CONFIG);
    dbus_server_uninit();
    database_uninit();
}

static gboolean
on_sigint(gpointer user_data G_GNUC_UNUSED)
{
    exit_handler();
    return G_SOURCE_REMOVE;
}

static void
server_set_config_add_wayland_seat(
    ConfigWaylandDisplay *config_dpy, ConfigWaylandSeat *config_seat,
    WaylandSeat *seat
)
{
    ClipporClipboard *cb = NULL;

    // Find clipboard with given name
    for (uint j = 0; j < CLIPBOARDS->len; j++)
    {
        cb = CLIPBOARDS->pdata[j];

        if (g_strcmp0(
                clippor_clipboard_get_label(cb), config_seat->clipboard
            ) == 0)
            break;
    }

    if (cb == NULL)
        return;

    g_autofree char *label =
        g_strdup_printf("%s_%s", config_dpy->display, config_seat->name);

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

        ClipporClipboard *cb = clippor_clipboard_new(config_cb.name);

        if (cb == NULL)
            continue;

        g_object_set(
            cb, "max-entries", config_cb.max_entries, "max-entries-memory",
            config_cb.max_entries_memory, "allowed-mime-types",
            config_cb.allowed_mime_types, "mime-type-groups",
            config_cb.mime_type_groups, NULL
        );

        g_ptr_array_add(CLIPBOARDS, cb);
    }

    // Get Wayland displays and add seats to their respective clipboard
    for (uint i = 0; i < CONFIG->wayland_displays->len; i++)
    {
        ConfigWaylandDisplay config_dpy =
            g_array_index(CONFIG->wayland_displays, ConfigWaylandDisplay, i);

        GError *error = NULL;
        WaylandConnection *ct =
            wayland_connection_new(config_dpy.display, &error);

        if (ct == NULL)
        {
            g_warning("%s", error->message);
            g_error_free(error);
        }

        g_ptr_array_add(WAYLAND_CONNECTIONS, ct);

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
                wayland_connection_get_seat(ct, config_seat.name);

            if (seat == NULL)
                continue;

            server_set_config_add_wayland_seat(&config_dpy, &config_seat, seat);
        }

        wayland_connection_install_source(ct, MAIN_CONTEXT);
    }
}

/*
 * Should only be called once within program lifetime
 */
gboolean
server_start(void)
{
    MAIN_CONTEXT = g_main_context_default();
    MAIN_LOOP = g_main_loop_new(MAIN_CONTEXT, FALSE);

    GError *error = NULL;

    if ((CONFIG = config_init(&error)) == NULL)
        goto fail;
    if (!database_init(&error))
        goto fail;
    if (CONFIG->dbus_service &&
        !dbus_service_init(&error, CONFIG->dbus_timeout))
        goto fail;

    CLIPBOARDS = g_ptr_array_new_with_free_func(g_object_unref);
    WAYLAND_CONNECTIONS = g_ptr_array_new_with_free_func(g_object_unref);

    server_set_config();

    g_unix_signal_add(SIGINT, on_sigint, NULL);
    g_unix_signal_add(SIGTERM, on_sigint, NULL);

    g_main_loop_run(MAIN_LOOP);

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

const GPtrArray *
server_get_clipboards(void)
{
    return CLIPBOARDS;
}
