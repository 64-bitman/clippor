#include "clippor-client.h"
#include "clippor-clipboard.h"
#include "clippor-entry.h"
#include "database.h"
#include "server.h"
#include "test.h"
#include <glib.h>
#include <locale.h>

/*
 * Test if clipboard is created correctly on startup
 */
static void
test_clipboard_start(void)
{

    server_instance_start(
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

    ClipporClipboard *cb = server_get_clipboard("Default");
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

    server_instance_stop();
}

/*
 * Test if a new selection is correctly received from a single Wayland
 * connection
 */
static void
test_clipboard_history_simple(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
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
        wc->display
    );

    server_instance_start(config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    // Check if both regular and selections are synced
    for (int i = 0; i < 2; i++)
    {
        g_autofree char *copy = g_strdup_printf("TEST %d", i);
        wl_copy(wc, i, "text/plain", copy);

        server_instance_dispatch();

        g_autoptr(ClipporEntry) entry =
            clippor_clipboard_get_entry(cb, i, &error);
        g_assert_no_error(error);

        g_assert_true(
            database_entry_id_exists(clippor_entry_get_id(entry), &error) == 0
        );
        g_assert_no_error(error);

        // Test if database is OK
        g_autoptr(ClipporEntry) d_entry =
            database_get_entry_by_id(cb, clippor_entry_get_id(entry), &error);
        g_assert_no_error(error);

        cmp_entry(entry, d_entry);

        // Run the server so it can serve the clipboard
        server_instance_run();

        // Check if opposite selection is the same
        g_autofree char *paste = wl_paste(wc, !i, FALSE, "text/plain");

        g_assert_cmpstr(paste, ==, copy);

        // Check if selection is still owned by the wl-copy process
        ClipporClient *client = clippor_entry_is_from(entry);

        g_assert_nonnull(client);
        g_assert_false(clippor_client_owns_selection(
            client, i == 0 ? CLIPPOR_SELECTION_TYPE_REGULAR
                           : CLIPPOR_SELECTION_TYPE_PRIMARY
        ));
        g_object_unref(client);

        server_instance_pause();
    }

    server_instance_stop();
    wayland_compositor_stop(wc);
}

/*
 * Test if a new selection is correctly received from multiple Wayland
 * connections
 */
static void
test_clipboard_history_multiple_connections(void)
{

    WaylandCompositor *wc = wayland_compositor_start();
    WaylandCompositor *wc2 = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
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
        "primary = false\n",
        wc->display, wc2->display
    );

    server_instance_start(config_contents);

    ClipporClipboard *cb = server_get_clipboard("Default");
    GError *error = NULL;
    ClipporEntry *entry;
    ClipporClient *client;

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    char *contents;

    wl_copy(wc, FALSE, "text/plain", "TEST 1");

    server_instance_dispatch();

    entry = clippor_clipboard_get_entry(cb, 0, &error);
    g_assert_no_error(error);

    client = clippor_entry_is_from(entry);
    g_assert_nonnull(client);

    g_assert_false(
        clippor_client_owns_selection(client, CLIPPOR_SELECTION_TYPE_REGULAR)
    );

    g_object_unref(client);

    server_instance_run();

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

    server_instance_pause();

    wl_copy(wc2, FALSE, "text/plain", "TEST 2");

    server_instance_dispatch();

    entry = clippor_clipboard_get_entry(cb, 0, &error);
    g_assert_no_error(error);

    ClipporClient *old_client = client;
    client = clippor_entry_is_from(entry);
    g_assert_nonnull(client);
    g_assert_true(client != old_client);

    g_assert_false(
        clippor_client_owns_selection(client, CLIPPOR_SELECTION_TYPE_REGULAR)
    );

    g_object_unref(client);

    server_instance_run();

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

    server_instance_pause();

    wl_copy(wc2, TRUE, "text/plain", "TEST 2");

    server_instance_dispatch_and_run();

    g_assert_cmpstr(
        (contents = wl_paste(wc, FALSE, FALSE, "text/plain")), ==, "TEST 2"
    );
    g_free(contents);

    server_instance_pause();

    server_instance_stop();
    wayland_compositor_stop(wc);
    wayland_compositor_stop(wc2);
}

/*
 * Test if an existing selection is correctly received on startup and that
 * clippor does not immediately set the selection.
 */
static void
test_clipboard_history_startup(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
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
        wc->display
    );

    wl_copy(wc, FALSE, "text/plain", "TEST");

    server_instance_start(config_contents);

    ClipporClipboard *cb = server_get_clipboard("Default");

    g_autoptr(ClipporEntry) entry = NULL;

    // For some reason sometimes entry is NULL so just dispatch until its we get
    // the entry.
    while ((entry = clippor_clipboard_get_entry(cb, 0, NULL)) == NULL)
        server_instance_dispatch();

    g_autofree char *copy = get_entry_contents(entry, "text/plain");

    g_assert_cmpstr(copy, ==, "TEST");

    g_autoptr(ClipporClient) client = clippor_entry_is_from(entry);

    g_assert_nonnull(client);
    g_assert_false(
        clippor_client_owns_selection(client, CLIPPOR_SELECTION_TYPE_REGULAR)
    );

    server_instance_stop();
    wayland_compositor_stop(wc);
}

/*
 * Test if history is trimmed to the correct amount when there are excess
 * entries.
 */
static void
test_clipboard_history_trim(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 10\n"
        "max_entries_memory = 5\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n",
        wc->display
    );

    server_instance_start(config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    for (int i = 0; i < 15; i++)
    {
        wl_copy(wc, FALSE, "text/plain", "TEST %d", i);
        server_instance_dispatch();
    }

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

    server_instance_stop();
    wayland_compositor_stop(wc);
}

/*
 * Test if starred entries are not trimmed from the history.
 */
static void
test_clipboard_history_starred(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 5\n"
        "max_entries_memory = 2\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n",
        wc->display
    );

    server_instance_start(config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, "text/plain", "TEST STARRED");

    server_instance_dispatch();

    g_autoptr(ClipporEntry) entry = clippor_clipboard_get_entry(cb, 0, NULL);

    clippor_entry_update_property(entry, &error, "starred", TRUE);
    g_assert_no_error(error);

    for (int i = 0; i < 7; i++)
    {
        wl_copy(wc, FALSE, "text/plain", "TEST %d", i);
        server_instance_dispatch();
    }

    g_autoptr(ClipporEntry) d_entry = clippor_clipboard_get_entry_by_id(
        cb, clippor_entry_get_id(entry), &error
    );
    g_assert_no_error(error);

    cmp_entry(entry, d_entry);

    server_instance_stop();
    wayland_compositor_stop(wc);
}

/*
 * Test if entry persists after a restart via the database.
 */
static void
test_clipboard_history_persists(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 5\n"
        "max_entries_memory = 2\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n",
        wc->display
    );

    server_instance_start(config_contents);

    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, "text/plain", "TEST");

    server_instance_dispatch();

    g_autoptr(ClipporEntry) entry = clippor_clipboard_get_entry(cb, 0, NULL);

    server_instance_restart();

    cb = server_get_clipboard("Default");
    g_autoptr(ClipporEntry) d_entry = clippor_clipboard_get_entry(cb, 0, NULL);

    g_assert_nonnull(d_entry);

    cmp_entry(entry, d_entry);

    server_instance_stop();
    wayland_compositor_stop(wc);
}

/*
 * Test if multiple clipboards behave correctly (i.e. not get mixed up).
 */
static void
test_clipboard_history_multiple_clipboards(void)
{
    WaylandCompositor *wc = wayland_compositor_start();
    WaylandCompositor *wc2 = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
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
        "primary = true\n",
        wc->display, wc2->display
    );

    server_instance_start(config_contents);

    ClipporClipboard *cb1 = server_get_clipboard("one");
    ClipporClipboard *cb2 = server_get_clipboard("two");

    g_assert_null(clippor_clipboard_get_entry(cb1, 0, NULL));
    g_assert_null(clippor_clipboard_get_entry(cb2, 0, NULL));

    wl_copy(wc, FALSE, "text/plain", "ONE REGULAR");
    server_instance_dispatch();
    wl_copy(wc, TRUE, "text/plain", "TWO PRIMARY");
    server_instance_dispatch();

    g_autoptr(ClipporEntry) entry1 = NULL, entry2 = NULL, entry3 = NULL,
                            entry4 = NULL;

    entry1 = clippor_clipboard_get_entry(cb1, 0, NULL);
    entry2 = clippor_clipboard_get_entry(cb2, 0, NULL);

    char *cb1_contents = get_entry_contents(entry1, "text/plain");
    char *cb2_contents = get_entry_contents(entry2, "text/plain");

    g_assert_cmpstr(cb1_contents, ==, "ONE REGULAR");
    g_assert_cmpstr(cb2_contents, ==, "TWO PRIMARY");
    g_free(cb1_contents);
    g_free(cb2_contents);

    wl_copy(wc2, FALSE, "text/plain", "ONE REGULAR2");

    server_instance_dispatch();

    entry3 = clippor_clipboard_get_entry(cb1, 0, NULL);
    cb1_contents = get_entry_contents(entry3, "text/plain");

    g_assert_cmpstr(cb1_contents, ==, "ONE REGULAR2");
    g_free(cb1_contents);

    wl_copy(wc2, TRUE, "text/plain", "ONE PRIMARY2");

    server_instance_dispatch();

    entry4 = clippor_clipboard_get_entry(cb1, 0, NULL);
    cb1_contents = get_entry_contents(entry4, "text/plain");

    g_assert_cmpstr(cb1_contents, ==, "ONE PRIMARY2");
    g_free(cb1_contents);

    server_instance_restart();

    // Check if they are correctly stored in database
    cb1 = server_get_clipboard("one");
    cb2 = server_get_clipboard("two");

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

    server_instance_stop();
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
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 5\n"
        "max_entries_memory = 2\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n",
        wc->display
    );

    server_instance_start(config_contents);

    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, "text/plain", "TEST");
    server_instance_dispatch();
    wl_copy(wc, TRUE, "text/plain", "TESTP");
    server_instance_dispatch();

    g_autoptr(ClipporEntry) entry = clippor_clipboard_get_entry(cb, 0, NULL);
    g_assert_nonnull(entry);

    wl_copy(wc, FALSE, NULL, NULL);
    server_instance_dispatch();
    wl_copy(wc, TRUE, NULL, NULL);
    server_instance_dispatch();

    server_instance_run();

    g_autofree char *paste = wl_paste(wc, FALSE, FALSE, "text/plain");

    server_instance_pause();

    g_assert_null(wl_paste(wc, TRUE, FALSE, "text/plain"));
    g_assert_cmpstr(paste, ==, "TEST");

    server_instance_stop();
    wayland_compositor_stop(wc);
}

/*
 * Test if removing an entry from history works correctly
 */
static void
test_clipboard_history_remove(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 3\n"
        "max_entries_memory = 1\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = false\n",
        wc->display
    );

    server_instance_start(config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, "text/plain", "MEMORY");
    server_instance_dispatch();
    wl_copy(wc, FALSE, "text/plain", "DATABASE STARRED");
    server_instance_dispatch();
    wl_copy(wc, FALSE, "text/plain", "DATABASE");
    server_instance_dispatch();

    ClipporEntry *entry = clippor_clipboard_get_entry(cb, 0, &error);

    g_assert_nonnull(entry);

    clippor_entry_update_property(entry, &error, "starred", TRUE, NULL);
    g_assert_no_error(error);

    g_assert_cmpint(database_get_num_entries(cb, &error), ==, 3);
    g_assert_no_error(error);

    clippor_clipboard_remove_entry(cb, clippor_entry_get_id(entry), &error);
    g_assert_no_error(error);

    g_assert_cmpint(database_get_num_entries(cb, &error), ==, 2);
    g_assert_no_error(error);

    server_instance_stop();
    wayland_compositor_stop(wc);
}

/*
 * Test if clearing the history works properly
 */
static void
test_clipboard_history_clear(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
        "[[clipboards]]\n"
        "clipboard = \"Default\"\n"
        "max_entries = 5\n"
        "max_entries_memory = 2\n"
        "[[wayland_displays]]\n"
        "display = \"%s\"\n"
        "[wayland_displays.seats]\n"
        "clipboard = \"Default\"\n"
        "regular = true\n"
        "primary = true\n",
        wc->display
    );

    server_instance_start(config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    for (int i = 0; i < 5; i++)
    {
        wl_copy(wc, i % 2, "text/plain", "TEST %d", i);
        server_instance_dispatch();
    }

    g_assert_cmpint(database_get_num_entries(cb, &error), ==, 5);

    clippor_clipboard_clear_history(cb, &error);
    g_assert_no_error(error);

    g_assert_cmpint(database_get_num_entries(cb, &error), ==, 0);
    g_assert_no_error(error);

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    server_instance_stop();
    wayland_compositor_stop(wc);
}

/*
 * Test if allowed_mime_types config option works properly
 */
static void
test_clipboard_filter_allowed_mime_types(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
        "dbus_service = false\n"
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
        "primary = true\n",
        wc->display
    );

    server_instance_start(config_contents);

    GError *error = NULL;
    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, NULL, "TEST");

    server_instance_dispatch();

    g_autoptr(ClipporEntry) entry = clippor_clipboard_get_entry(cb, 0, &error);
    g_assert_no_error(error);

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);

    g_assert_cmpint(g_hash_table_size(mime_types), ==, 2);

    server_instance_stop();
    wayland_compositor_stop(wc);
}

static void
test_clipboard_filter_mime_type_groups(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    g_autofree char *config_contents = g_strdup_printf(
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
        "primary = true\n",
        wc->display
    );

    server_instance_start(config_contents);

    ClipporClipboard *cb = server_get_clipboard("Default");

    g_assert_null(clippor_clipboard_get_entry(cb, 0, NULL));

    wl_copy(wc, FALSE, "text/plain;charset=utf-8", "test");

    server_instance_dispatch();

    g_autoptr(ClipporEntry) entry = clippor_clipboard_get_entry(cb, 0, NULL);

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);

    g_assert_cmpint(g_hash_table_size(mime_types), ==, 5);

    server_instance_stop();
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

    g_test_add_func("/clipboard/start", test_clipboard_start);
    g_test_add_func("/clipboard/history/simple", test_clipboard_history_simple);
    g_test_add_func(
        "/clipboard/history/multiple-connections",
        test_clipboard_history_multiple_connections
    );
    g_test_add_func(
        "/clipboard/history/startup", test_clipboard_history_startup
    );
    g_test_add_func("/clipboard/history/trim", test_clipboard_history_trim);
    g_test_add_func(
        "/clipboard/history/starred", test_clipboard_history_starred
    );
    g_test_add_func(
        "/clipboard/history/persists", test_clipboard_history_persists
    );
    g_test_add_func(
        "/clipboard/history/multiple-clipboards",
        test_clipboard_history_multiple_clipboards
    );
    g_test_add_func(
        "/clipboard/history/own-on-clear", test_clipboard_history_own_on_clear
    );
    g_test_add_func("/clipboard/history/remove", test_clipboard_history_remove);
    g_test_add_func("/clipboard/history/clear", test_clipboard_history_clear);
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
