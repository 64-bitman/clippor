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

ClipporServer *
start_server(void)
{
    ClipporServer *server = g_new(ClipporServer, 1);

    server->context = g_main_context_default();
    server->loop = g_main_loop_new(server->context, FALSE);
    server->settings = g_settings_new("com.github.64bitman.clippor");

    server->ct = wayland_connection_new("");

    wayland_connection_set_status(server->ct, TRUE);
    wayland_connection_install_source(server->ct);

    server->seat = wayland_connection_get_seat(server->ct, NULL);

    wayland_seat_set_status(server->seat, TRUE);

    server->cb = clippor_clipboard_new("Untitled");

    clippor_clipboard_add_client(
        server->cb, G_OBJECT(server->seat), "Test", "clipboard-regular",
        "selection::regular"
    );

    g_unix_signal_add(SIGINT, on_sigint, server);

    g_main_loop_run(server->loop);

    return server;
}

void
stop_server(ClipporServer *server)
{
    g_main_loop_quit(server->loop);

    wayland_connection_uninstall_source(server->ct);

    g_object_unref(server->cb);
    g_object_unref(server->ct);

    g_object_unref(server->settings);

    g_main_loop_unref(server->loop);
    g_main_context_unref(server->context);

    g_free(server);
}
