#include "config.h"
#include <glib-unix.h>
#include <glib.h>
#include <locale.h>

typedef struct
{
    const char *config_path;
    const char *config_contents;
    GError *error;
    Config *config;
} ConfigFixture;

static void
config_fixture_setup(ConfigFixture *fixture, gconstpointer user_data)
{
    fixture->config_contents = user_data;
    fixture->config_path = "test_config.toml";

    GError *error = NULL;
    gboolean ret = g_file_set_contents(
        fixture->config_path, fixture->config_contents, -1, &error
    );

    if (!ret)
        g_error("%s", error->message);

    fixture->config = config_init(fixture->config_path, &fixture->error);
}

static void
config_fixture_teardown(
    ConfigFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
)
{
    if (fixture->error != NULL)
        g_error_free(fixture->error);
    if (fixture->config != NULL)
        config_free(fixture->config);
    g_unlink(fixture->config_path);
}

/*
 * Test if config is correctly initialized given a config file
 */
static void
test_config_full_valid(
    ConfigFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
)
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
test_config_all_seats(
    ConfigFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
)
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
test_config_invalid(
    ConfigFixture *fixture G_GNUC_UNUSED, gconstpointer user_data G_GNUC_UNUSED
)
{
    g_assert_error(fixture->error, CONFIG_ERROR, CONFIG_ERROR_INVALID);
}

/*
 * Test if regex for Wayland seat works correctly
 */
static void
test_config_seat_regex(
    ConfigFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
)
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

    g_test_add(
        "/config/valid/full", ConfigFixture,
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
        "primary = false\n",
        config_fixture_setup, test_config_full_valid, config_fixture_teardown
    );
    g_test_add(
        "/config/valid/all-seats", ConfigFixture,
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n",
        config_fixture_setup, test_config_all_seats, config_fixture_teardown
    );
    g_test_add(
        "/config/invalid/no_cb_name", ConfigFixture,
        "[[clipboards]]\n"
        "max_entries = 10\n",
        config_fixture_setup, test_config_invalid, config_fixture_teardown
    );
    g_test_add(
        "/config/invalid/no_display_name", ConfigFixture,
        "[[wayland_displays]]\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n",
        config_fixture_setup, test_config_invalid, config_fixture_teardown
    );
    g_test_add(
        "/config/invalid/no-seat-name", ConfigFixture,
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[[wayland_displays.seats]]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n",
        config_fixture_setup, test_config_invalid, config_fixture_teardown
    );
    g_test_add(
        "/config/invalid/no-seat-clipboard", ConfigFixture,
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[[wayland_displays.seats]]\n"
        "seat = \"seat0\"\n"
        "regular = true\n"
        "primary = false\n",
        config_fixture_setup, test_config_invalid, config_fixture_teardown
    );
    g_test_add(
        "/config/invalid/dbus-timeout-too-small", ConfigFixture,
        "dbus_timeout = -2\n", config_fixture_setup, test_config_invalid,
        config_fixture_teardown
    );

    g_test_add(
        "/config/invalid/max-entries-zero", ConfigFixture,
        "[[clipboards]]\n"
        "clipboard = \"CB\"\n"
        "max_entries = 0\n"
        "max_entries_memory = 5\n"
        "allowed_mime_types = []\n"
        "mime_type_groups = []\n",
        config_fixture_setup, test_config_invalid, config_fixture_teardown
    );

    g_test_add(
        "/config/invalid/max-untries-memory-zero", ConfigFixture,
        "[[clipboards]]\n"
        "clipboard = \"CB\"\n"
        "max_entries = 10\n"
        "max_entries_memory = 0\n"
        "allowed_mime_types = []\n"
        "mime_type_groups = []\n",
        config_fixture_setup, test_config_invalid, config_fixture_teardown
    );

    g_test_add(
        "/config/invalid/connection-timeout-too-small", ConfigFixture,
        "[[wayland_displays]]\n"
        "display = \"DISPLAY\"\n"
        "connection_timeout = -2\n"
        "data_timeout = 100\n",
        config_fixture_setup, test_config_invalid, config_fixture_teardown
    );

    g_test_add(
        "/config/invalid/data-timeout-too-small", ConfigFixture,
        "[[wayland_displays]]\n"
        "display = \"DISPLAY\"\n"
        "connection_timeout = 100\n"
        "data_timeout = -5\n",
        config_fixture_setup, test_config_invalid, config_fixture_teardown
    );
    g_test_add(
        "/config/seat-regex", ConfigFixture,
        "[[wayland_displays]]\n"
        "display = \"$WAYLAND_DISPLAY\"\n"
        "connection_timeout = 3000\n"
        "data_timeout = 1500\n"
        "[[wayland_displays.seats]]\n"
        "seat = \"seat.*\"\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n",
        config_fixture_setup, test_config_seat_regex, config_fixture_teardown
    );

    return g_test_run();
}
