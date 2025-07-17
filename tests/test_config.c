#include "config.h"
#include "test.h"
#include <glib-unix.h>
#include <glib.h>
#include <locale.h>

typedef struct
{
    GError *error;
    Config *config;
} TestFixture;

static void
fixture_setup(TEST_AARGS)
{
    fixture->config = config_init(user_data, FALSE, &fixture->error);
}

static void
fixture_teardown(TEST_ARGS)
{
    if (fixture->error != NULL)
        g_error_free(fixture->error);
    if (fixture->config != NULL)
        config_free(fixture->config);
}

/*
 * Test if error if non existement config file given
 */
static void
test_config_nonexistent_file(void)
{
    GError *error = NULL;

    g_assert_false(config_init("RANDOM FILE", TRUE, &error));
    g_assert_error(error, CONFIG_ERROR, CONFIG_ERROR_NO_FILE);

    g_error_free(error);
}

/*
 * Test if config is correctly initialized given a config file
 */
static void
test_config_full_valid(TEST_ARGS)
{
    g_assert_no_error(fixture->error);

    Config *cfg = fixture->config;

    g_assert_cmpint(cfg->dbus_timeout, ==, 1000);
    g_assert_cmpint(cfg->dbus_service, ==, TRUE);

    ConfigClipboard cb = g_array_index(cfg->clipboards, ConfigClipboard, 0);

    g_assert_cmpstr(cb.name, ==, "Default");
    g_assert_cmpint(cb.max_entries, ==, 10);
    g_assert_cmpint(cb.max_entries_memory, ==, 5);

    g_assert_cmpint(cb.allowed_mime_types->len, ==, 2);

    GHashTableIter iter;
    GPtrArray *group;

    g_hash_table_iter_init(&iter, cb.mime_type_groups);

    g_hash_table_iter_next(&iter, NULL, (gpointer *)&group);

    g_assert_nonnull(group);
    g_assert_cmpint(group->len, ==, 4);

    g_assert_cmpstr(group->pdata[0], ==, "TEXT");
    g_assert_cmpstr(group->pdata[1], ==, "STRING");
    g_assert_cmpstr(group->pdata[2], ==, "UTF8_STRING");
    g_assert_cmpstr(group->pdata[3], ==, "text/plain");

    ConfigWaylandDisplay wayland_dpy =
        g_array_index(cfg->wayland_displays, ConfigWaylandDisplay, 0);

    g_assert_cmpstr(wayland_dpy.display, ==, getenv("WAYLAND_DISPLAY"));
    g_assert_cmpint(wayland_dpy.connection_timeout, ==, 3000);
    g_assert_cmpint(wayland_dpy.data_timeout, ==, 1500);

    ConfigWaylandSeat seat =
        g_array_index(wayland_dpy.seats, ConfigWaylandSeat, 0);

    g_assert_cmpstr(seat.clipboard, ==, "Default");
    g_assert_cmpint(seat.regular, ==, TRUE);
    g_assert_cmpint(seat.primary, ==, FALSE);
}

/*
 * Test if the seats member of a wayland display table that is a table instead
 * of an array of tables is correctly picked up as to use all Wayland seats.
 */
static void
test_config_all_seats(TEST_ARGS)
{
    g_assert_no_error(fixture->error);

    Config *cfg = fixture->config;

    ConfigWaylandDisplay wayland_dpy =
        g_array_index(cfg->wayland_displays, ConfigWaylandDisplay, 0);
    ConfigWaylandSeat seat =
        g_array_index(wayland_dpy.seats, ConfigWaylandSeat, 0);

    g_assert_null(seat.name);
}

/*
 * Test if config returns an error on an invalid configuration.
 */
static void
test_config_invalid(TEST_UARGS)
{
    g_assert_error(fixture->error, CONFIG_ERROR, CONFIG_ERROR_INVALID);
}

/*
 * Test if regex for Wayland seat works correctly
 */
static void
test_config_seat_regex(TEST_ARGS)
{
    g_assert_no_error(fixture->error);

    Config *cfg = fixture->config;

    ConfigWaylandDisplay wayland_dpy =
        g_array_index(cfg->wayland_displays, ConfigWaylandDisplay, 0);
    ConfigWaylandSeat seat =
        g_array_index(wayland_dpy.seats, ConfigWaylandSeat, 0);

    g_assert_true(
        g_regex_match(seat.name, "seat0", G_REGEX_MATCH_DEFAULT, NULL)
    );
    g_assert_false(
        g_regex_match(seat.name, "test", G_REGEX_MATCH_DEFAULT, NULL)
    );
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_test_init(&argc, &argv, NULL);

    pre_startup();

    struct sigaction sa;
    set_signal_handler(&sa);

    g_test_add_func("/config/nonexistent-file", test_config_nonexistent_file);
    TEST_ADD_DATA(
        "/config/valid/full", test_config_full_valid,
        "dbus_timeout = 1000\n"
        "dbus_service = true\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 10\n"
        "max_entries_memory = 5\n"
        "allowed_mime_types = [ \"text/*\", \"image/*\" ]\n"
        "[[clipboards.mime_type_groups]]\n"
        "mime_type = \"text/plain;charset=utf-8\"\n"
        "group = [ \"TEXT\", \"STRING\", \"UTF8_STRING\", \"text/plain\" ]\n"
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[[wayland_displays.seats]]\n"
        "seat = \"seat0\"\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n"
    );

    TEST_ADD_DATA(
        "/config/valid/all-seats", test_config_all_seats,
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/no_cb_name", test_config_invalid,
        "[[clipboards]]\n"
        "max_entries = 10\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/no_display_name", test_config_invalid,
        "[[wayland_displays]]\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/no-seat-name", test_config_invalid,
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[[wayland_displays.seats]]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/no-seat-clipboard", test_config_invalid,
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[[wayland_displays.seats]]\n"
        "seat = \"seat0\"\n"
        "regular = true\n"
        "primary = false\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/dbus-timeout-too-small", test_config_invalid,
        "dbus_timeout = -2\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/max-entries-zero", test_config_invalid,
        "[[clipboards]]\n"
        "clipboard = \"CB\"\n"
        "max_entries = 0\n"
        "max_entries_memory = 5\n"
        "allowed_mime_types = []\n"
        "mime_type_groups = []\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/max-untries-memory-zero", test_config_invalid,
        "[[clipboards]]\n"
        "clipboard = \"CB\"\n"
        "max_entries = 10\n"
        "max_entries_memory = 0\n"
        "allowed_mime_types = []\n"
        "mime_type_groups = []\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/connection-timeout-too-small", test_config_invalid,
        "[[wayland_displays]]\n"
        "display = \"DISPLAY\"\n"
        "connection_timeout = -2\n"
        "data_timeout = 100\n"
    );

    TEST_ADD_DATA(
        "/config/invalid/data-timeout-too-small", test_config_invalid,
        "[[wayland_displays]]\n"
        "display = \"DISPLAY\"\n"
        "connection_timeout = 100\n"
        "data_timeout = -5\n"
    );

    TEST_ADD_DATA(
        "/config/seat-regex", test_config_seat_regex,
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[[wayland_displays.seats]]\n"
        "seat = \"seat.*\"\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n"
    );

    return g_test_run();
}
