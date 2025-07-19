#include "com.github.clippor.h"
#include "server.h"
#include "test.h"
#include <gio/gio.h>
#include <gio/gtestdbus.h>
#include <glib.h>
#include <locale.h>

// GTestDBus unsets $XDG_RUNTIME_DIR, so we must set it again each time
static const char *XDG_RUNTIME_DIR;

typedef struct
{
    GTestDBus *dbus;
    GDBusObjectManager *primary;
    GDBusObjectManager *clipboards;
    GDBusObjectManager *wayland_cts;

    BusClippor *p_interface;
    BusClipporWaylandConnection *wc_interface;
    BusClipporWaylandConnection *wc2_interface;

    WaylandCompositor *wc;
    WaylandCompositor *wc2;
} TestFixture;

/*
 * Dispatch events on the global main default context
 */
static void
context_dispatch(void)
{
    // DBus objects will use the global default context because g_test_dbus_new
    // is called before server_start().
    while (g_main_context_pending(g_main_context_default()))
        g_main_context_iteration(g_main_context_default(), TRUE);
}

static void
fixture_setup(TEST_ARGS)
{
    fixture->dbus = g_test_dbus_new(G_TEST_DBUS_NONE);

    g_test_dbus_up(fixture->dbus);

    g_setenv("XDG_RUNTIME_DIR", XDG_RUNTIME_DIR, TRUE);

    fixture->wc = wayland_compositor_start();
    fixture->wc2 = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = true\n"
        "[[clipboards]]\n"
        "clipboard = \"One\"\n"
        "max_entries = 10\n"
        "max_entries_memory = 5\n"
        "[[clipboards]]\n"
        "clipboard = \"Two\"\n"
        "max_entries = 10\n"
        "max_entries_memory = 5\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"One\"\n"
        "regular = true\n"
        "primary = true\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[[wayland_displays.seats]]\n"
        "seat = \".*\"\n"
        "clipboard = \"Two\"\n"
        "regular = true\n"
        "primary = false\n"
        "[[wayland_displays.seats]]\n"
        "seat = \".*\"\n"
        "clipboard = \"One\"\n"
        "regular = false\n"
        "primary = true\n",
        fixture->wc->display, fixture->wc2->display
    );

    server_instance_start(config_contents);
    server_instance_run();

    GError *error = NULL;

    fixture->primary = bus_object_manager_client_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        "com.github.Clippor", "/com/github", NULL, &error
    );
    g_assert_no_error(error);

    fixture->clipboards = bus_object_manager_client_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        "com.github.Clippor", "/com/github/Clippor/Clipboards", NULL, &error
    );
    g_assert_no_error(error);

    fixture->wayland_cts = bus_object_manager_client_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        "com.github.Clippor", "/com/github/Clippor/WaylandConnections", NULL,
        &error
    );
    g_assert_no_error(error);

    fixture->p_interface = bus_clippor_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, "com.github.Clippor",
        "/com/github/Clippor", NULL, &error
    );
    g_assert_no_error(error);

    char *path;

    path = replace_dbus_illegal_chars(
        fixture->wc->display, "/com/github/Clippor/WaylandConnections"
    );
    fixture->wc_interface =
        bus_clippor_wayland_connection_proxy_new_for_bus_sync(
            G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, "com.github.Clippor",
            path, NULL, &error
        );
    g_free(path);
    g_assert_no_error(error);

    path = replace_dbus_illegal_chars(
        fixture->wc2->display, "/com/github/Clippor/WaylandConnections"
    );
    fixture->wc2_interface =
        bus_clippor_wayland_connection_proxy_new_for_bus_sync(
            G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, "com.github.Clippor",
            path, NULL, &error
        );
    g_free(path);
    g_assert_no_error(error);
}

static void
fixture_teardown(TEST_ARGS)
{
    g_object_unref(fixture->wc_interface);
    g_object_unref(fixture->wc2_interface);
    g_object_unref(fixture->p_interface);
    g_object_unref(fixture->primary);
    g_object_unref(fixture->clipboards);
    g_object_unref(fixture->wayland_cts);

    server_instance_pause();
    server_instance_stop();

    wayland_compositor_stop(fixture->wc);
    wayland_compositor_stop(fixture->wc2);

    g_test_dbus_down(fixture->dbus);
    g_object_unref(fixture->dbus);
}

/*
 * Test if dbus service starts up correctly
 */
static void
test_dbus_startup(TEST_ARGS)
{
    GList *p_objects = g_dbus_object_manager_get_objects(fixture->primary);
    GList *cb_objects = g_dbus_object_manager_get_objects(fixture->clipboards);
    GList *ct_objects = g_dbus_object_manager_get_objects(fixture->wayland_cts);

    // Check if objects are exported
    g_assert_cmpint(g_list_length(p_objects), ==, 1);
    g_assert_cmpint(g_list_length(cb_objects), ==, 2);
    g_assert_cmpint(g_list_length(ct_objects), ==, 2);

    g_list_free_full(p_objects, g_object_unref);
    g_list_free_full(cb_objects, g_object_unref);
    g_list_free_full(ct_objects, g_object_unref);
}

/*
 * Check if clipboards can be listed
 */
static void
test_dbus_clippor_list_clipboards(TEST_ARGS)
{
    GError *error = NULL;
    char **names;

    bus_clippor_call_list_clipboards_sync(
        fixture->p_interface, &names, NULL, &error
    );
    g_assert_no_error(error);

    g_assert_cmpint(g_strv_length(names), ==, 2);
    g_assert_true(g_strv_contains((const char **)names, "One"));
    g_assert_true(g_strv_contains((const char **)names, "Two"));

    g_strfreev(names);
}

static void
test_dbus_clippor_add_clipboard(TEST_ARGS)
{
    GError *error = NULL;

    bus_clippor_call_add_clipboard_sync(
        fixture->p_interface, "Three", NULL, &error
    );
    g_assert_no_error(error);

    bus_clippor_call_add_clipboard_sync(
        fixture->p_interface, "One", NULL, &error
    );
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_DBUS_ERROR);
    g_clear_error(&error);

    server_instance_pause();

    g_assert_nonnull(server_get_clipboard("Three"));

    server_instance_run();

    context_dispatch();

    // Test if object is exported
    g_autoptr(GDBusObject) obj = g_dbus_object_manager_get_object(
        fixture->clipboards, "/com/github/Clippor/Clipboards/Three"
    );

    g_assert_nonnull(obj);
}

static void
test_dbus_clippor_list_wayland_connections(TEST_ARGS)
{
    GError *error = NULL;
    char **names;

    bus_clippor_call_list_wayland_connections_sync(
        fixture->p_interface, &names, NULL, &error
    );
    g_assert_no_error(error);

    g_assert_cmpint(g_strv_length(names), ==, 2);
    g_assert_true(g_strv_contains((const char **)names, fixture->wc->display));
    g_assert_true(g_strv_contains((const char **)names, fixture->wc2->display));

    g_strfreev(names);
}

static void
test_dbus_clippor_add_wayland_connection(TEST_ARGS)
{
    GError *error = NULL;
    g_autoptr(WaylandCompositor) wc = wayland_compositor_start();

    bus_clippor_call_add_wayland_connection_sync(
        fixture->p_interface, wc->display, NULL, &error
    );
    g_assert_no_error(error);

    server_instance_pause();

    g_assert_nonnull(server_get_wayland_connection(wc->display));

    server_instance_run();

    context_dispatch();

    g_autofree char *path = replace_dbus_illegal_chars(
        wc->display, "/com/github/Clippor/WaylandConnections"
    );

    // Test if object is exported
    g_autoptr(GDBusObject) obj =
        g_dbus_object_manager_get_object(fixture->wayland_cts, path);

    g_assert_nonnull(obj);
}

static void
test_dbus_wayland_connections_list_seats(TEST_ARGS)
{
    GError *error = NULL;
    char **names;

    bus_clippor_wayland_connection_call_list_seats_sync(
        fixture->wc_interface, &names, NULL, &error
    );
    g_assert_no_error(error);

    g_assert_cmpint(g_strv_length(names), >, 0);

    g_strfreev(names);
}

static void
test_dbus_wayland_connections_connect_seat(TEST_ARGS)
{
    GError *error = NULL;
    g_autoptr(WaylandCompositor) wc = wayland_compositor_start();

    bus_clippor_call_add_wayland_connection_sync(
        fixture->p_interface, wc->display, NULL, &error
    );
    g_assert_no_error(error);

    context_dispatch();
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_test_init(&argc, &argv, NULL);

    pre_startup();

    struct sigaction sa;
    set_signal_handler(&sa);

    XDG_RUNTIME_DIR = g_getenv("XDG_RUNTIME_DIR");

    TEST_ADD("/dbus/startup", test_dbus_startup);
    TEST_ADD(
        "/dbus/clippor/list-clipboards", test_dbus_clippor_list_clipboards
    );
    TEST_ADD("/dbus/clippor/add-clipboard", test_dbus_clippor_add_clipboard);
    TEST_ADD(
        "/dbus/clippor/list-wayland-connections",
        test_dbus_clippor_list_wayland_connections
    );
    TEST_ADD(
        "/dbus/clippor/add-wayland-connection",
        test_dbus_clippor_add_wayland_connection
    );

    TEST_ADD(
        "/dbus/wayland-connections/list-seats",
        test_dbus_wayland_connections_list_seats
    );
    TEST_ADD(
        "/dbus/wayland-connections/connect-seat",
        test_dbus_wayland_connections_connect_seat
    );

    return g_test_run();
}
