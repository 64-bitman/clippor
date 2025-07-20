#include "server.h"
#include "config.h"
#include <glib-unix.h>
#include <glib.h>

static uint SIGNAL_SOURCE_IDS[2] = {0};
static GMainLoop *LOOP;

static GPtrArray *CLIPBOARDS;
static GPtrArray *WAYLAND_CONNECTIONS;

static Config *CONFIG;

// Handle SIGTERM and SIGINT signals
static gboolean
handle_signals(gpointer user_data G_GNUC_UNUSED)
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

    // Handle signals so we can exit cleanly
    SIGNAL_SOURCE_IDS[0] = g_unix_signal_add(SIGINT, handle_signals, NULL);
    SIGNAL_SOURCE_IDS[1] = g_unix_signal_add(SIGTERM, handle_signals, NULL);

    g_main_loop_run(LOOP);
    g_main_loop_unref(LOOP);

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
