#include "global.h"
#include "wayland-connection.h"
#include "wayland-seat.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <locale.h>
#include <stdlib.h>

static gboolean opt_version = FALSE;
static gboolean opt_server = FALSE;
static gboolean opt_debug = FALSE;

static GOptionEntry help_entries[] = {
    {"version", 'v', 0, G_OPTION_ARG_NONE, &opt_version, "Print version", NULL},
    {"server", 's', 0, G_OPTION_ARG_NONE, &opt_server, "Serve the clipboard",
     NULL},
    {"debug", 'd', 0, G_OPTION_ARG_NONE, &opt_debug, "Enable verbose logging",
     NULL},
    G_OPTION_ENTRY_NULL
};

typedef struct
{
    GMainContext *main_context;
    GMainLoop *main_loop;
    GSettings *settings;

    GPtrArray *clipboards;
    GPtrArray *connections;
    GPtrArray *clients;
} GlobalState;

static gboolean on_sigint(gpointer user_data);
static void weak_ref_cb(gpointer data, GObject *object);

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    GError *error = NULL;
    GOptionContext *context = g_option_context_new("- clipboard manager");

    // Read commandline
    g_option_context_add_main_entries(context, help_entries, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_option_context_free(context);
        goto exit;
    }
    g_option_context_free(context);

    // Serve some command line flags
    if (opt_version)
    {
        g_print("Clipstuff version " VERSION "\n");
        goto exit;
    }
    if (opt_debug)
        g_log_set_debug_enabled(TRUE);

    if (opt_server)
    {
        GlobalState *state = g_new(GlobalState, 1);

        state->main_context = g_main_context_default();
        state->main_loop = g_main_loop_new(state->main_context, FALSE);
        state->settings = g_settings_new("com.github.64bitman.clippor");

        SETTINGS = state->settings;

        state->clipboards = g_ptr_array_new_full(2, g_object_unref);
        state->connections = g_ptr_array_new_full(2, NULL);
        state->clients = g_ptr_array_new_full(2, NULL);

        // Get the default Wayland client using $WAYLAND_DISPLAY
        WaylandConnection *ct = wayland_connection_new(NULL, &error);

        if (ct == NULL)
            goto exit;

        wayland_connection_install_source(ct, state->main_context);

        g_object_weak_ref(G_OBJECT(ct), weak_ref_cb, state->connections);
        g_ptr_array_add(state->connections, ct);

        // Create default clipboard
        ClipporClipboard *cb = clippor_clipboard_new("Default");

        g_ptr_array_add(state->clipboards, cb);

        GHashTableIter iter;
        WaylandSeat *seat;

        g_hash_table_iter_init(&iter, wayland_connection_get_seats(ct));

        // Add seats to array
        while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&seat))
        {
            g_object_weak_ref(G_OBJECT(seat), weak_ref_cb, state->clients);
            g_ptr_array_add(state->clients, seat);

            clippor_clipboard_add_client(
                cb, wayland_seat_get_name(seat), CLIPPOR_CLIENT(seat),
                CLIPPOR_SELECTION_TYPE_REGULAR
            );
        }

        g_unix_signal_add(SIGINT, on_sigint, state);

        g_main_loop_run(state->main_loop);
    }

exit:
    if (error != NULL)
    {
        g_critical("%s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static gboolean
on_sigint(gpointer user_data)
{
    GlobalState *state = user_data;

    g_message("Exiting...");

    g_main_loop_quit(state->main_loop);
    g_main_loop_unref(state->main_loop);
    g_object_unref(state->settings);

    g_ptr_array_unref(state->clipboards);
    g_ptr_array_unref(state->connections);
    g_ptr_array_unref(state->clients);

    g_free(state);

    return G_SOURCE_REMOVE;
}

static void
weak_ref_cb(gpointer data, GObject *object)
{
    g_ptr_array_remove(data, object);
}
