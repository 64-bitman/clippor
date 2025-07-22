#include "server.h"
#include "clippor-clipboard.h"
#include "clippor-database.h"
#include "config.h"
#include "wayland-connection.h"
#include <glib-unix.h>
#include <glib.h>
#include <sqlite3.h>

G_DEFINE_QUARK(DATABASE_ERROR, database_error)

static uint SIGNAL_SOURCE_IDS[2] = {0};
static GMainLoop *LOOP;

static GPtrArray *CLIPBOARDS;
static GPtrArray *WAYLAND_CONNECTIONS;

static Config *CONFIG;

// Handle SIGTERM and SIGINT signals
static gboolean
handle_signals(void *user_data G_GNUC_UNUSED)
{
    g_message("Exiting...");

    // Remove source ids
    for (uint i = 0; i < G_N_ELEMENTS(SIGNAL_SOURCE_IDS); i++)
        g_source_remove(SIGNAL_SOURCE_IDS[i]);

    g_main_loop_quit(LOOP);

    return G_SOURCE_REMOVE;
}

/*
 * Create and initialize required objects so that server can be run
 */
static gboolean
server_setup(GError **error)
{
    g_assert(error == NULL || *error == NULL);

    CLIPBOARDS = g_ptr_array_ref(CONFIG->clipboards);
    WAYLAND_CONNECTIONS = g_ptr_array_ref(CONFIG->wayland_connections);

    return TRUE;
}

/*
 * Start the server normally
 */
gboolean
server_start(const char *config_file, const char *data_directory)
{
    GError *error = NULL;

    if ((CONFIG = config_new_file(config_file, &error)) == NULL)
        goto fail;
    (void)data_directory;

    if (!server_setup(&error))
        goto fail;

    LOOP = g_main_loop_new(g_main_context_get_thread_default(), FALSE);

    ClipporDatabase *db =
        clippor_database_new(NULL, CLIPPOR_DATABASE_DEFAULT, &error);

    if (db == NULL)
    {
        g_warning("Failed initializing database: %s", error->message);
    }

    ClipporClipboard *cb = clippor_clipboard_new("Default", db);
    WaylandConnection *ct = wayland_connection_new("wayland-1");

    if (!clippor_clipboard_load(cb, &error))
    {
        g_warning("%s", error->message);
    }

    wayland_connection_start(ct, NULL);
    wayland_connection_install_source(ct, g_main_context_get_thread_default());

    WaylandSeat *seat = wayland_connection_get_seat(ct, NULL);
    ClipporSelection *sel = CLIPPOR_SELECTION(
        wayland_seat_get_selection(seat, CLIPPOR_SELECTION_TYPE_REGULAR)
    );

    clippor_clipboard_add_selection(cb, sel);

        // Handle signals so we can exit cleanly
        SIGNAL_SOURCE_IDS[0] = g_unix_signal_add(SIGINT, handle_signals, NULL);
    SIGNAL_SOURCE_IDS[1] = g_unix_signal_add(SIGTERM, handle_signals, NULL);

    g_main_loop_run(LOOP);
    g_main_loop_unref(LOOP);

    g_object_unref(db);
    g_object_unref(ct);
    g_object_unref(cb);

    server_free();

    return TRUE;
fail:
    g_warning("Failed starting server: %s", error->message);
    g_error_free(error);

    return FALSE;
}

/*
 * Free all resources created by server session
 */
void
server_free(void)
{
    config_free(CONFIG);

    g_ptr_array_unref(CLIPBOARDS);
    g_ptr_array_unref(WAYLAND_CONNECTIONS);
}
