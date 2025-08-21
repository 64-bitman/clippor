#include "test.h"
#include "wayland-connection.h"
#include <glib.h>
#include <locale.h>

typedef struct
{
    GMainContext *context;
} TestFixture;

static void
test_fixture_setup(TEST_ARGS)
{
    fixture->context = g_main_context_new();

    g_main_context_push_thread_default(fixture->context);
}

static void
test_fixture_teardown(TEST_ARGS)
{
    g_main_context_pop_thread_default(fixture->context);
    g_main_context_unref(fixture->context);
}

/*
 * Test if Wayland events from connection are handled correctly
 */
static void
test_wayland_connection_events(TEST_ARGS)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(WaylandCompositor) wc = wayland_compositor_new();
    g_autoptr(WaylandConnection) ct = wayland_connection_new(wc->display);

    g_assert_nonnull(ct);
    wayland_connection_install_source(ct, fixture->context);

    wayland_connection_start(ct, &error);
    g_assert_no_error(error);

    g_autoptr(WaylandSeat) seat = wayland_connection_get_seat(ct, NULL);

    g_assert_nonnull(seat);

    g_autoptr(WaylandDataDeviceManager) manager =
        wayland_connection_get_data_device_manager(ct);

    g_assert_nonnull(manager);

    g_autoptr(WaylandDataDevice) device =
        wayland_data_device_manager_get_data_device(manager, seat);
}

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    g_test_init(&argc, &argv, NULL);

    test_setup();

    TEST("/wayland/connection/events", test_wayland_connection_events);

    return g_test_run();
}
