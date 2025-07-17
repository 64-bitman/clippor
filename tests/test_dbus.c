#include "test.h"
#include <gio/gio.h>
#include <gio/gtestdbus.h>
#include <glib.h>
#include <locale.h>

typedef struct
{
    GTestDBus *dbus;
    WaylandCompositor *wc;
} TestFixture;

static void
fixture_setup(TEST_ARGS)
{
    // GTestDBus unsets $XDG_RUNTIME_DIR
    const char *xdgruntimedir = g_getenv("XDG_RUNTIME_DIR");

    fixture->dbus = g_test_dbus_new(G_TEST_DBUS_NONE);

    g_test_dbus_up(fixture->dbus);

    g_setenv("XDG_RUNTIME_DIR", xdgruntimedir, TRUE);

    fixture->wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = true\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 10\n"
        "max_entries_memory = 5\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = true\n",
        fixture->wc->display
    );

    server_instance_start(config_contents);
    server_instance_run();
}

static void
fixture_teardown(TEST_ARGS)
{
    server_instance_pause();
    server_instance_stop();
    wayland_compositor_stop(fixture->wc);

    g_test_dbus_down(fixture->dbus);
    g_object_unref(fixture->dbus);
}

static void
test_dbus_startup(TEST_UARGS)
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

    TEST_ADD("/dbus/startup", test_dbus_startup);

    return g_test_run();
}
