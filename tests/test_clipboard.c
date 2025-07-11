#include "clippor-clipboard.h"
#include "database.h"
#include "server.h"
#include "spawn.h"
#include "util.h"
#include <glib.h>
#include <locale.h>

/*
 * Test if clipboard is created correctly on startup
 */
static void
test_clipboard_start(void)
{

    ServerInstance *server = run_server(
        "clipboard",
        "dbus_service = false\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 10\n"
        "max_entries_memory = 5\n"
        "allowed_mime_types = [ \"text/*\", \"image/*\" ]\n"
        "[[clipboards.mime_type_groups]]\n"
        "mime_type = \"text/plain;charset=utf-8\"\n"
        "group = [ \"TEXT\", \"STRING\", \"UTF8_STRING\", \"text/plain\" ]\n"
    );

    ClipporClipboard *cb = server_get_clipboards()->pdata[0];
    int64_t max_entries, max_entries_memory;

    g_assert_nonnull(cb);

    g_object_get(
        cb, "max-entries", &max_entries, "max-entries-memory",
        &max_entries_memory, NULL
    );

    g_assert_cmpint(max_entries, ==, 10);
    g_assert_cmpint(max_entries_memory, ==, 5);

    g_autoptr(GPtrArray) allowed_mime_types;
    g_autoptr(GHashTable) mime_type_groups;

    g_object_get(
        cb, "allowed-mime-types", &allowed_mime_types, "mime-type-groups",
        &mime_type_groups, NULL
    );

    g_assert_nonnull(allowed_mime_types);
    g_assert_nonnull(mime_type_groups);
    g_assert_nonnull(allowed_mime_types->pdata[0]);
    g_assert_nonnull(allowed_mime_types->pdata[1]);

    g_assert_true(g_regex_match(
        allowed_mime_types->pdata[0], "text/plain", G_REGEX_MATCH_DEFAULT, NULL
    ));
    g_assert_true(g_regex_match(
        allowed_mime_types->pdata[1], "image/plain", G_REGEX_MATCH_DEFAULT, NULL
    ));

    GHashTableIter iter;
    GRegex *mime_type;
    GPtrArray *group;

    g_hash_table_iter_init(&iter, mime_type_groups);
    g_hash_table_iter_next(&iter, (gpointer *)&mime_type, (gpointer *)&group);

    g_assert_nonnull(mime_type);
    g_assert_true(g_regex_match(
        mime_type, "text/plain;charset=utf-8", G_REGEX_MATCH_DEFAULT, NULL
    ));
    g_assert_nonnull(group);
    g_assert_cmpint(group->len, ==, 4);

    g_assert_true(stop_server(server));
}

/*
 * Compare if two entries are the same
 */
static void
cmp_entry(ClipporEntry *entry, ClipporEntry *entry2)
{
    GError *error = NULL;

    g_assert_cmpstr(
        clippor_entry_get_id(entry2), ==, clippor_entry_get_id(entry)
    );
    g_assert_cmpint(
        clippor_entry_get_creation_time(entry2), ==,
        clippor_entry_get_creation_time(entry)
    );
    g_assert_cmpint(
        clippor_entry_get_last_used_time(entry2), ==,
        clippor_entry_get_last_used_time(entry)
    );
    g_assert_cmpint(
        clippor_entry_is_starred(entry2), ==, clippor_entry_is_starred(entry)
    );

    GHashTable *d_mime_types = clippor_entry_get_mime_types(entry2);
    GHashTable *mime_types = clippor_entry_get_mime_types(entry);

    GHashTableIter iter;
    const char *mime_type;

    g_hash_table_iter_init(&iter, d_mime_types);

    while (g_hash_table_iter_next(&iter, (gpointer *)&mime_type, NULL))
        g_assert_true(g_hash_table_contains(mime_types, mime_type));

    g_autoptr(ClipporData) d_data = NULL, data = NULL;

    d_data = clippor_entry_get_data(entry2, "text/plain", &error);
    g_assert_no_error(error);
    data = clippor_entry_get_data(entry, "text/plain", &error);
    g_assert_no_error(error);

    size_t d_sz, sz;
    const char *d_raw, *raw;

    d_raw = clippor_data_get_data(d_data, &d_sz);
    raw = clippor_data_get_data(data, &sz);

    g_autofree char *d_str = NULL, *str = NULL;

    d_str = g_strdup_printf("%.*s", (int)d_sz, d_raw);
    str = g_strdup_printf("%.*s", (int)sz, raw);

    g_assert_cmpstr(d_str, ==, str);
}

/*
 * Return allocated string of entry data for mime type
 */
static char *
get_entry_contents(ClipporEntry *entry, const char *mime_type)
{

    GError *error = NULL;
    g_autoptr(ClipporData) data = NULL;

    g_assert_no_error(error);
    data = clippor_entry_get_data(entry, mime_type, &error);
    g_assert_no_error(error);

    size_t sz;
    const char *raw;

    raw = clippor_data_get_data(data, &sz);

    return g_strdup_printf("%.*s", (int)sz, raw);
}

/*
 * Test if a new selection is correctly received from a single Wayland
 * connection
 */
static void
test_clipboard_new_entry_single(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"Default\"\n"
                                      "max_entries = 10\n"
                                      "max_entries_memory = 5\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = true\n";

    WaylandCompositor *wc = wayland_compositor_start();

    char *config_contents = g_strdup_printf(config_contents_fmt, wc->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    // Check if both regular and selections are synced
    for (int i = 0; i < 2; i++)
    {

        g_autofree char *copy = g_strdup_printf("TEST %d", i);
        wl_copy(wc, i, copy);

        g_autoptr(ClipporEntry) entry = NULL;

        assert_wait(
            (entry = clippor_clipboard_get_entry(cb, i, NULL)), != NULL,
            nonnull, 3000
        );

        g_assert_true(
            database_entry_id_exists(clippor_entry_get_id(entry), &error) == 0
        );
        g_assert_no_error(error);

        g_autoptr(ClipporEntry) d_entry =
            database_get_entry_by_id(cb, clippor_entry_get_id(entry), &error);
        g_assert_no_error(error);

        cmp_entry(entry, d_entry);

        g_autofree char *paste = NULL;

        // Check if opposite selection is the same
        paste = wl_paste(wc, !i, FALSE, "text/plain");

        g_assert_cmpstr(paste, ==, copy);
    }

    g_free(config_contents);
    wayland_compositor_stop(wc);
    g_assert_true(stop_server(server));
}

/*
 * Test if a new selection is correctly received from multiple Wayland
 * connections
 */
static void
test_clipboard_new_entry_multi(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"Default\"\n"
                                      "max_entries = 10\n"
                                      "max_entries_memory = 5\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = true\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = false\n";

    WaylandCompositor *wc = wayland_compositor_start();
    WaylandCompositor *wc2 = wayland_compositor_start();

    char *config_contents =
        g_strdup_printf(config_contents_fmt, wc->display, wc2->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    char *contents;

    wl_copy(wc, FALSE, "TEST 1");

    g_assert_cmpstr(
        (contents = wl_paste(wc, FALSE, FALSE, "text/plain")), ==, "TEST 1"
    );
    g_free(contents);
    g_assert_cmpstr(
        (contents = wl_paste(wc2, FALSE, FALSE, "text/plain")), ==, "TEST 1"
    );
    g_free(contents);
    g_assert_cmpstr(
        (contents = wl_paste(wc, TRUE, FALSE, "text/plain")), ==, "TEST 1"
    );
    g_free(contents);
    g_assert_cmpstr(
        (contents = wl_paste(wc2, TRUE, FALSE, "text/plain")), ==, NULL
    );
    g_free(contents);

    wl_copy(wc2, FALSE, "TEST 2");

    g_usleep(100 * 1000);
    g_assert_cmpstr(
        (contents = wl_paste(wc, FALSE, FALSE, "text/plain")), ==, "TEST 2"
    );
    g_free(contents);
    g_assert_cmpstr(
        (contents = wl_paste(wc2, FALSE, FALSE, "text/plain")), ==, "TEST 2"
    );
    g_free(contents);
    g_assert_cmpstr(
        (contents = wl_paste(wc, TRUE, FALSE, "text/plain")), ==, "TEST 2"
    );
    g_free(contents);
    g_assert_cmpstr(
        (contents = wl_paste(wc2, TRUE, FALSE, "text/plain")), ==, NULL
    );
    g_free(contents);

    wl_copy(wc2, TRUE, "TEST 2");

    g_assert_cmpstr(
        (contents = wl_paste(wc, FALSE, FALSE, "text/plain")), ==, "TEST 2"
    );
    g_free(contents);

    g_free(config_contents);
    wayland_compositor_stop(wc);
    wayland_compositor_stop(wc2);
    g_assert_true(stop_server(server));
}

/*
 * Test if an existing selection is correctly received on startup
 */
static void
test_clipboard_new_entry_pre(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"Default\"\n"
                                      "max_entries = 10\n"
                                      "max_entries_memory = 5\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = true\n";

    WaylandCompositor *wc = wayland_compositor_start();

    char *config_contents = g_strdup_printf(config_contents_fmt, wc->display);

    wl_copy(wc, FALSE, "TEST");

    ServerInstance *server = run_server("clipboard", config_contents);

    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    g_free(config_contents);
    g_assert_true(stop_server(server));
    wayland_compositor_stop(wc);
}

/*
 * Test if history is trimmed to the correct amount when there are excess
 * entries.
 */
static void
test_clipboard_history_trim(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"Default\"\n"
                                      "max_entries = 10\n"
                                      "max_entries_memory = 5\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = false\n";

    WaylandCompositor *wc = wayland_compositor_start();

    char *config_contents = g_strdup_printf(config_contents_fmt, wc->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    for (int i = 0; i < 15; i++)
    {
        g_autofree char *copy = g_strdup_printf("TEST %d", i);

        wl_copy(wc, FALSE, copy);

        // Dont copy too fast to the point where its faster than the data being
        // sent over the pipe.
    }

    g_usleep(100 * 1000);
    for (int i = 0; i < 10; i++)
    {
        g_autofree char *copy = g_strdup_printf("TEST %d", 14 - i);
        g_autoptr(ClipporEntry) entry =
            clippor_clipboard_get_entry(cb, i, &error);

        g_assert_nonnull(entry);
        g_assert_no_error(error);

        g_autoptr(ClipporData) data =
            clippor_entry_get_data(entry, "text/plain", &error);

        g_assert_no_error(error);

        size_t sz;
        const char *raw;

        raw = clippor_data_get_data(data, &sz);

        g_autofree char *str = g_strdup_printf("%.*s", (int)sz, raw);

        g_assert_cmpstr(str, ==, copy);
    }

    int64_t num = database_get_num_entries(cb, &error);

    g_assert_no_error(error);
    g_assert_cmpint(num, ==, 10);

    g_free(config_contents);
    g_assert_true(stop_server(server));
    wayland_compositor_stop(wc);
}

/*
 * Test if starred entries are not trimmed from the history.
 */
static void
test_clipboard_history_starred(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"Default\"\n"
                                      "max_entries = 5\n"
                                      "max_entries_memory = 2\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = false\n";

    WaylandCompositor *wc = wayland_compositor_start();

    char *config_contents = g_strdup_printf(config_contents_fmt, wc->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, "TEST STARRED");

    g_autoptr(ClipporEntry) entry = NULL;

    assert_wait(
        (entry = clippor_clipboard_get_entry(cb, 0, NULL)), != NULL, nonnull,
        3000
    );

    clippor_entry_update_property(entry, &error, "starred", TRUE);
    g_assert_no_error(error);

    for (int i = 0; i < 7; i++)
    {
        g_autofree char *copy = g_strdup_printf("TEST %d", i);

        wl_copy(wc, FALSE, copy);
    }

    g_autoptr(ClipporEntry) d_entry = clippor_clipboard_get_entry_by_id(
        cb, clippor_entry_get_id(entry), &error
    );
    g_assert_no_error(error);

    cmp_entry(entry, d_entry);

    g_free(config_contents);
    g_assert_true(stop_server(server));
    wayland_compositor_stop(wc);
}

/*
 * Test if entry persists after a restart via the database.
 */
static void
test_clipboard_history_persists(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"Default\"\n"
                                      "max_entries = 5\n"
                                      "max_entries_memory = 2\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = false\n";

    WaylandCompositor *wc = wayland_compositor_start();

    char *config_contents = g_strdup_printf(config_contents_fmt, wc->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, "TEST");

    g_autoptr(ClipporEntry) entry = NULL;

    assert_wait(
        (entry = clippor_clipboard_get_entry(cb, 0, NULL)), != NULL, nonnull,
        3000
    );

    g_assert_true(restart_server(server));

    cb = server_get_clipboards()->pdata[0];
    g_autoptr(ClipporEntry) d_entry = clippor_clipboard_get_entry(cb, 0, NULL);

    g_assert_nonnull(d_entry);

    cmp_entry(entry, d_entry);

    g_free(config_contents);
    wayland_compositor_stop(wc);
    g_assert_true(stop_server(server));
}

/*
 * Test if multiple clipboards behave correctly (i.e. not get mixed up).
 */
static void
test_clipboard_history_multi(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"one\"\n"
                                      "max_entries = 5\n"
                                      "max_entries_memory = 2\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"two\"\n"
                                      "max_entries = 5\n"
                                      "max_entries_memory = 2\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[[wayland_displays.seats]]\n"
                                      "seat = \".*\"\n"
                                      "clipboard = \"one\"\n"
                                      "regular = true\n"
                                      "primary = false\n"
                                      "[[wayland_displays.seats]]\n"
                                      "seat = \".*\"\n"
                                      "clipboard = \"two\"\n"
                                      "regular = false\n"
                                      "primary = true\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[[wayland_displays.seats]]\n"
                                      "seat = \".*\"\n"
                                      "clipboard = \"one\"\n"
                                      "regular = true\n"
                                      "primary = true\n";

    WaylandCompositor *wc = wayland_compositor_start();
    WaylandCompositor *wc2 = wayland_compositor_start();

    char *config_contents =
        g_strdup_printf(config_contents_fmt, wc->display, wc2->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    ClipporClipboard *cb1 = server_get_clipboards()->pdata[0];
    ClipporClipboard *cb2 = server_get_clipboards()->pdata[1];

    g_assert_null(clippor_clipboard_get_entry(cb1, 0, NULL));
    g_assert_null(clippor_clipboard_get_entry(cb2, 0, NULL));

    wl_copy(wc, FALSE, "ONE REGULAR");
    wl_copy(wc, TRUE, "TWO PRIMARY");

    g_autoptr(ClipporEntry) entry1 = NULL, entry2 = NULL, entry3 = NULL,
                            entry4 = NULL;

    assert_wait(
        (entry1 = clippor_clipboard_get_entry(cb1, 0, NULL)), != NULL, nonnull,
        3000
    );
    assert_wait(
        (entry2 = clippor_clipboard_get_entry(cb2, 0, NULL)), != NULL, nonnull,
        3000
    );

    char *cb1_contents = get_entry_contents(entry1, "text/plain");
    char *cb2_contents = get_entry_contents(entry2, "text/plain");

    g_assert_cmpstr(cb1_contents, ==, "ONE REGULAR");
    g_assert_cmpstr(cb2_contents, ==, "TWO PRIMARY");
    g_free(cb1_contents);
    g_free(cb2_contents);

    wl_copy(wc2, FALSE, "ONE REGULAR2");

    // Must sleep because using assert_wait will return immediately becuase
    // index 0 still refers to the previous entry.
    g_usleep(100 * 1000);
    entry3 = clippor_clipboard_get_entry(cb1, 0, NULL);
    cb1_contents = get_entry_contents(entry3, "text/plain");

    g_assert_cmpstr(cb1_contents, ==, "ONE REGULAR2");
    g_free(cb1_contents);

    wl_copy(wc2, TRUE, "ONE PRIMARY2");

    g_usleep(100 * 1000);
    entry4 = clippor_clipboard_get_entry(cb1, 0, NULL);
    cb1_contents = get_entry_contents(entry4, "text/plain");

    g_assert_cmpstr(cb1_contents, ==, "ONE PRIMARY2");
    g_free(cb1_contents);

    g_assert_true(restart_server(server));

    // Check if they are correctly stored in database
    cb1 = server_get_clipboards()->pdata[0];
    cb2 = server_get_clipboards()->pdata[1];

    g_autoptr(ClipporEntry) d_entry1 = NULL, d_entry2 = NULL, d_entry3 = NULL;

    d_entry1 = clippor_clipboard_get_entry(cb1, 0, NULL);
    g_assert_nonnull(d_entry1);
    d_entry2 = clippor_clipboard_get_entry(cb1, 1, NULL);
    g_assert_nonnull(d_entry2);
    d_entry3 = clippor_clipboard_get_entry(cb1, 2, NULL);
    g_assert_nonnull(d_entry3);

    cmp_entry(entry4, d_entry1);
    cmp_entry(entry3, d_entry2);
    cmp_entry(entry1, d_entry3);

    g_free(config_contents);
    g_assert_true(stop_server(server));
    wayland_compositor_stop(wc);
    wayland_compositor_stop(wc2);
}

/*
 * Test if clipboard regains ownership of the selection and sets it to the
 * most recent selection before the selection was cleared. This means the
 * clipboard cannot never be empty if there are entries in history unless
 * manually set empty through the DBus interface.
 */
static void
test_clipboard_history_own_on_clear(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"Default\"\n"
                                      "max_entries = 5\n"
                                      "max_entries_memory = 2\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = false\n";

    WaylandCompositor *wc = wayland_compositor_start();

    char *config_contents = g_strdup_printf(config_contents_fmt, wc->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, "TEST");
    wl_copy(wc, TRUE, "TESTP");

    g_usleep(100 * 1000);

    wl_copy(wc, FALSE, NULL);
    wl_copy(wc, TRUE, NULL);

    g_usleep(100 * 1000);

    g_autofree char *paste = wl_paste(wc, FALSE, FALSE, "text/plain");

    g_assert_null(wl_paste(wc, TRUE, FALSE, "text/plain"));
    g_assert_cmpstr(paste, ==, "TEST");

    g_free(config_contents);
    wayland_compositor_stop(wc);
    g_assert_true(stop_server(server));
}

/*
 * Test if allowed_mime_types config option works properly
 */
static void
test_clipboard_filter_allowed_mime_types(void)
{
    const char *config_contents_fmt = "dbus_service = false\n"
                                      "[[clipboards]]\n"
                                      "clipboard = \"Default\"\n"
                                      "max_entries = 5\n"
                                      "max_entries_memory = 2\n"
                                      "allowed_mime_types = [ \"text/.*\" ]\n"
                                      "[[wayland_displays]]\n"
                                      "display = \"%s\"\n"
                                      "[wayland_displays.seats]\n"
                                      "clipboard = \"Default\"\n"
                                      "regular = true\n"
                                      "primary = true\n";

    WaylandCompositor *wc = wayland_compositor_start();

    char *config_contents = g_strdup_printf(config_contents_fmt, wc->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    g_autoptr(ClipporEntry) entry = NULL;

    wl_copy(wc, FALSE, "TEST");

    assert_wait(
        (entry = clippor_clipboard_get_entry(cb, 0, NULL)), != NULL, nonnull,
        3000
    );

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);

    g_assert_cmpint(g_hash_table_size(mime_types), ==, 2);

    g_free(config_contents);
    wayland_compositor_stop(wc);
    g_assert_true(stop_server(server));
}

static void
test_clipboard_filter_mime_type_groups(void)
{
    const char *config_contents_fmt =
        "dbus_service = false\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 5\n"
        "max_entries_memory = 2\n"
        "allowed_mime_types = [ \"text/plain;charset=utf-8\" ]\n"
        "[[clipboards.mime_type_groups]]\n"
        "mime_type = \"text/plain;charset=utf-8\"\n"
        "group = [ \"TEXT\", \"STRING\", \"UTF8_STRING\", \"text/plain\" ]\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = true\n";

    WaylandCompositor *wc = wayland_compositor_start();

    char *config_contents = g_strdup_printf(config_contents_fmt, wc->display);

    ServerInstance *server = run_server("clipboard", config_contents);

    ClipporClipboard *cb = server_get_clipboards()->pdata[0];

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    g_autoptr(ClipporEntry) entry = NULL;

    wl_copy(wc, FALSE, "test");

    assert_wait(
        (entry = clippor_clipboard_get_entry(cb, 0, NULL)), != NULL, nonnull,
        3000
    );

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);

    g_assert_cmpint(g_hash_table_size(mime_types), ==, 5);

    g_free(config_contents);
    wayland_compositor_stop(wc);
    g_assert_true(stop_server(server));
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_test_init(&argc, &argv, NULL);

    struct sigaction sa;
    set_signal_handler(&sa);

    g_test_add_func("/clipboard/start", test_clipboard_start);
    g_test_add_func(
        "/clipboard/new-entry/single", test_clipboard_new_entry_single
    );
    g_test_add_func(
        "/clipboard/new-entry/multi", test_clipboard_new_entry_multi
    );
    g_test_add_func("/clipboard/new-entry/pre", test_clipboard_new_entry_pre);
    g_test_add_func("/clipboard/history/trim", test_clipboard_history_trim);
    g_test_add_func(
        "/clipboard/history/starred", test_clipboard_history_starred
    );
    g_test_add_func(
        "/clipboard/history/persists", test_clipboard_history_persists
    );
    g_test_add_func("/clipboard/history/multi", test_clipboard_history_multi);
    g_test_add_func(
        "/clipboard/history/own-on-clear", test_clipboard_history_own_on_clear
    );
    g_test_add_func(
        "/clipboard/filter/allowed-mime-types",
        test_clipboard_filter_allowed_mime_types
    );
    g_test_add_func(
        "/clipboard/filter/mime-type-groups",
        test_clipboard_filter_mime_type_groups
    );

    return g_test_run();
}
