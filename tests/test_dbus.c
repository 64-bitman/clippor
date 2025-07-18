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

    BusClippor *p_interface;

    WaylandCompositor *wc;
    WaylandCompositor *wc2;
} TestFixture;

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
        "com.github.clippor", "/com/github", NULL, &error
    );
    g_assert_no_error(error);

    fixture->clipboards = bus_object_manager_client_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
        "com.github.clippor", "/com/github/clippor/clipboards", NULL, &error
    );
    g_assert_no_error(error);

    fixture->p_interface = bus_clippor_proxy_new_for_bus_sync(
        G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, "com.github.clippor",
        "/com/github/clippor", NULL, &error
    );
    g_assert_no_error(error);
}

static void
fixture_teardown(TEST_ARGS)
{
    g_object_unref(fixture->p_interface);
    g_object_unref(fixture->primary);
    g_object_unref(fixture->clipboards);

    server_instance_pause();
    server_instance_stop();

    wayland_compositor_stop(fixture->wc);
    wayland_compositor_stop(fixture->wc2);

    // Fixes g_test_dbus_down hanging because there are still objects
    // referencing the bus connection. I'm guessing this works because the
    // underlying logic for the above unref calls may not be synchronous?
    while (g_main_context_pending(g_main_context_get_thread_default()))
        g_main_context_iteration(g_main_context_get_thread_default(), TRUE);

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

    // Check if objects are exported
    g_assert_cmpint(g_list_length(p_objects), ==, 1);
    g_assert_cmpint(g_list_length(cb_objects), ==, 2);
    g_list_free_full(cb_objects, g_object_unref);
    g_list_free_full(p_objects, g_object_unref);
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

    ClipporClipboard *cb = server_get_clipboard("Three");

    g_assert_nonnull(cb);

    server_instance_run();
}

static void
test_dbus_clippor_list_wayland_connections(TEST_ARGS)
{
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

    return g_test_run();
}
