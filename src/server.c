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

static GMainContext *MAIN_CONTEXT;
static GMainLoop *MAIN_LOOP;

static GPtrArray *CLIPBOARDS;
static GPtrArray *WAYLAND_CONNECTIONS;

static Config *CONFIG;

static gboolean RUNNING = FALSE;

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

static void
server_free(void)
{
    if (!RUNNING)
        return;
    RUNNING = FALSE;

    g_main_loop_unref(MAIN_LOOP);
    g_ptr_array_unref(WAYLAND_CONNECTIONS);
    g_ptr_array_unref(CLIPBOARDS);

    config_free(CONFIG);
    dbus_server_uninit();
    database_uninit();
}

static gboolean
on_exit_signal(gpointer user_data G_GNUC_UNUSED)
{
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
            continue;
        }

        GWeakRef *ref = g_new(GWeakRef, 1);

        g_weak_ref_init(ref, ct);

        g_ptr_array_add(WAYLAND_CONNECTIONS, ref);

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

        wayland_connection_install_source(ct, MAIN_CONTEXT);
    }
}

gboolean
server_start(const char *config_file, const char *data_directory)
{
    if (RUNNING)
        return FALSE;

    MAIN_CONTEXT = g_main_context_default();
    MAIN_LOOP = g_main_loop_new(MAIN_CONTEXT, FALSE);

    GError *error = NULL;

    if ((CONFIG = config_init(config_file, &error)) == NULL)
        goto fail;
    if (!database_init(data_directory, &error))
        goto fail;
    if (CONFIG->dbus_service &&
        !dbus_service_init(&error, CONFIG->dbus_timeout))
        goto fail;

    CLIPBOARDS = g_ptr_array_new_with_free_func(g_object_unref);
    WAYLAND_CONNECTIONS = g_ptr_array_new_with_free_func(
        (GDestroyNotify)wayland_connections_free_func
    );

    server_set_config();

    g_unix_signal_add(SIGINT, on_exit_signal, NULL);
    g_unix_signal_add(SIGTERM, on_exit_signal, NULL);

    RUNNING = TRUE;

    g_main_loop_run(MAIN_LOOP);

    server_free();

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

gboolean
server_is_running(void)
{
    return RUNNING;
}
