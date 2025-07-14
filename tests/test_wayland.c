#include "test.h"
#include "wayland-connection.h"
#include <glib.h>
#include <locale.h>

static void
test_wayland_connection_startup(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    GError *error = NULL;
    WaylandConnection *ct = wayland_connection_new(wc->display, &error);

    g_assert_no_error(error);
    g_assert_cmpstr(wayland_connection_get_display_name(ct), ==, wc->display);

    wayland_compositor_stop(wc);
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_test_init(&argc, &argv, NULL);

    pre_startup();

    struct sigaction sa;
    set_signal_handler(&sa);

    g_test_add_func(
        "/wayland/connection/startup", test_wayland_connection_startup
    );

    return g_test_run();
}
