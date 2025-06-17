#include "wayland-connection.h"
#include <glib.h>
#include <locale.h>
#include <wayland-client.h>

typedef struct
{
    WaylandConnection *ct;
} WaylandConnectionFixture;

static void
wayland_connection_fixture_setup(
    WaylandConnectionFixture *fixture, gconstpointer data
)
{
    fixture->ct = wayland_connection_new((gchar *)data);
}

static void
wayland_connection_fixture_teardown(
    WaylandConnectionFixture *fixture, gconstpointer data G_GNUC_UNUSED
)
{
    g_clear_object(&fixture->ct);
}

static void
test_connection_start(
    WaylandConnectionFixture *fixture, gconstpointer data G_GNUC_UNUSED
)
{
    g_assert_true(wayland_connection_set_status(fixture->ct, TRUE));
    g_assert_true(wayland_connection_is_active(fixture->ct));
}

static void
test_connection_stop(
    WaylandConnectionFixture *fixture, gconstpointer data G_GNUC_UNUSED
)
{
    wayland_connection_set_status(fixture->ct, TRUE);
    g_assert_true(wayland_connection_is_active(fixture->ct));
    wayland_connection_set_status(fixture->ct, FALSE);
    g_assert_false(wayland_connection_is_active(fixture->ct));
}

static void
test_connection_invalid(
    WaylandConnectionFixture *fixture, gconstpointer data G_GNUC_UNUSED
)
{
    wayland_connection_set_status(fixture->ct, TRUE);

    g_assert_false(wayland_connection_set_display(fixture->ct, "UNKNOWN"));
    g_assert_false(wayland_connection_set_status(fixture->ct, TRUE));

    g_assert_true(
        wayland_connection_set_display(fixture->ct, g_getenv("WAYLAND_DISPLAY"))
    );
    g_assert_true(wayland_connection_set_status(fixture->ct, TRUE));
}

static void
test_connection_switch(
    WaylandConnectionFixture *fixture, gconstpointer data G_GNUC_UNUSED
)
{
    g_assert_true(wayland_connection_set_status(fixture->ct, TRUE));
    wayland_connection_set_display(fixture->ct, g_getenv("WAYLAND_DISPLAY2"));

    g_assert_true(wayland_connection_set_status(fixture->ct, TRUE));
    g_assert_true(
        wayland_connection_set_display(fixture->ct, g_getenv("WAYLAND_DISPLAY"))
    );
}

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    g_test_init(&argc, &argv, NULL);

    g_test_add(
        "/connection/start", WaylandConnectionFixture, "",
        wayland_connection_fixture_setup, test_connection_start,
        wayland_connection_fixture_teardown
    );
    g_test_add(
        "/connection/stop", WaylandConnectionFixture, "",
        wayland_connection_fixture_setup, test_connection_stop,
        wayland_connection_fixture_teardown
    );
    g_test_add(
        "/connection/invalid", WaylandConnectionFixture, "",
        wayland_connection_fixture_setup, test_connection_invalid,
        wayland_connection_fixture_teardown
    );
    g_test_add(
        "/connection/switch", WaylandConnectionFixture, "",
        wayland_connection_fixture_setup, test_connection_switch,
        wayland_connection_fixture_teardown
    );

    return g_test_run();
}
