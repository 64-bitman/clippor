#include "server.h"
#include "wayland-seat.h"
#include <glib-unix.h>
#include <glib.h>

gboolean
on_sigint(gpointer user_data)
{
    g_message("Exiting...");
    stop_server(user_data);
    return G_SOURCE_REMOVE;
}

gboolean
on_sigusr1(gpointer user_data)
{
    ClipporServer *server = user_data;
    g_object_unref(server->ct);

    return G_SOURCE_CONTINUE;
}

ClipporServer *
start_server(void)
{
    ClipporServer *server = g_new(ClipporServer, 1);

    server->context = g_main_context_default();
    server->loop = g_main_loop_new(server->context, FALSE);
    server->settings = g_settings_new("com.github.64bitman.clippor");

    server->ct = wayland_connection_new("", NULL);
    server->cb = clippor_clipboard_new("Untitled");

    clippor_clipboard_add_client(
        server->cb, "Untitled",
        CLIPPOR_CLIENT(wayland_connection_get_seat(server->ct, NULL)),
        CLIPPOR_SELECTION_TYPE_REGULAR
    );

    g_unix_signal_add(SIGINT, on_sigint, server);
    g_unix_signal_add(SIGUSR1, on_sigusr1, server);

    wayland_connection_install_source(server->ct, NULL);

    g_main_loop_run(server->loop);

    return server;
}

void
stop_server(ClipporServer *server)
{
    g_main_loop_quit(server->loop);

    g_object_unref(server->cb);
    g_object_unref(server->ct);

    /* g_object_unref(server->settings); */

    g_main_loop_unref(server->loop);
    g_main_context_unref(server->context);

    g_free(server);
}
