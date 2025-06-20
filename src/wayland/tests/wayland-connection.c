#include "wayland-connection.h"
#include "test_util.h"
#include <glib.h>
#include <locale.h>
#include <wayland-client.h>

static void
test_connection_new(void)
{
    WaylandCompositor *wc = wayland_compositor_new(TRUE);
    WaylandConnection *ct;

    // Explicit display passed
    ct = wayland_connection_new(wc->display, NULL);

    g_assert_true(ct != NULL);
    g_assert_cmpstr(wayland_connection_get_display_name(ct), ==, wc->display);
    g_object_unref(ct);

    // NULL display passed
    ct = wayland_connection_new(NULL, NULL);

    g_assert_true(ct != NULL);
    g_assert_cmpstr(wayland_connection_get_display_name(ct), ==, wc->display);
    g_object_unref(ct);

    ct = wayland_connection_new("", NULL);

    g_assert_true(ct != NULL);
    g_assert_cmpstr(wayland_connection_get_display_name(ct), ==, wc->display);
    g_object_unref(ct);

    // Invalid display passed
    ct = wayland_connection_new("UNKNOWN", NULL);

    g_assert_true(ct == NULL);

    wayland_compositor_destroy(wc);
}

static void
test_connection_seat(void)
{
    WaylandCompositor *wc = wayland_compositor_new(TRUE);
    WaylandConnection *ct = wayland_connection_new(wc->display, NULL);

    const gchar *prev_xdgseat = g_getenv("XDG_SEAT");

    // Explicit display passed

    g_assert_true(wayland_connection_get_seat(ct, NULL) != NULL);
    g_assert_true(wayland_connection_get_seat(ct, "") != NULL);

    g_unsetenv("XDG_SEAT");
    g_assert_true(wayland_connection_get_seat(ct, NULL) != NULL);

    g_setenv("XDG_SEAT", "UNKNOWN", TRUE);
    g_assert_true(wayland_connection_get_seat(ct, NULL) != NULL);

    g_object_unref(ct);
    g_setenv("XDG_SEAT", prev_xdgseat, TRUE);
    wayland_compositor_destroy(wc);
}

static gboolean
test_connection_lost_timer_callback(gpointer data)
{
    WaylandCompositor **wc = ((void **)data)[0];
    GMainLoop *loop = ((void **)data)[1];

    if (*wc == NULL)
    {
        g_main_loop_quit(loop);
        return G_SOURCE_REMOVE;
    }

    wayland_compositor_destroy(*wc);
    *wc = NULL;
    return G_SOURCE_CONTINUE;
    ;
}

static void
test_connection_lost(void)
{
    WaylandCompositor *wc = wayland_compositor_new(TRUE);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GWeakRef ref;

    g_weak_ref_init(&ref, wayland_connection_new(wc->display, NULL));

    WaylandConnection *ct = g_weak_ref_get(&ref);

    wayland_connection_install_source(ct, NULL);
    g_object_unref(ct);

    void *data[] = {&wc, loop};

    g_timeout_add(100, test_connection_lost_timer_callback, data);

    g_main_loop_run(loop);

    // Object should unref it self when connection is lost.
    g_assert_true(g_weak_ref_get(&ref) == NULL);

    g_main_loop_unref(loop);
}

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    struct sigaction sa;
    set_sigabrt_handler(&sa);

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/connection/new", test_connection_new);
    g_test_add_func("/connection/seat", test_connection_seat);
    g_test_add_func("/connection/lost", test_connection_lost);

    return g_test_run();
}
