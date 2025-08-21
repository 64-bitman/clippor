#include "clippor-clipboard.h"
#include "dummy-selection.h"
#include "test.h"
#include <glib.h>
#include <locale.h>

typedef struct
{
    GMainContext *context;
    ClipporClipboard *cb;
} TestFixture;

static void
test_fixture_setup(TEST_ARGS)
{
    fixture->context = g_main_context_new();
    fixture->cb = clippor_clipboard_new("TEST");

    g_main_context_push_thread_default(fixture->context);
}

static void
test_fixture_teardown(TEST_ARGS)
{
    g_main_context_pop_thread_default(fixture->context);

    g_main_context_unref(fixture->context);
    g_object_unref(fixture->cb);
}

/*
 * Test if entry is created when a selection is updated.
 */
static void
test_clipboard_update(TEST_ARGS)
{
    ClipporClipboard *cb = fixture->cb;
    g_autoptr(DummySelection) rsel =
        dummy_selection_new(CLIPPOR_SELECTION_TYPE_REGULAR);
    g_autoptr(DummySelection) psel =
        dummy_selection_new(CLIPPOR_SELECTION_TYPE_PRIMARY);

    clippor_clipboard_add_selection(cb, CLIPPOR_SELECTION(rsel));
    clippor_clipboard_add_selection(cb, CLIPPOR_SELECTION(psel));

    dummy_selection_install_source(rsel, fixture->context);
    dummy_selection_install_source(psel, fixture->context);

    dummy_selection_copy(rsel, "hello", "text/plain", "TEXT", NULL);

    main_context_dispatch(fixture->context);

    ClipporEntry *entry = clippor_clipboard_get_entry(cb);

    g_assert_nonnull(entry);

    // Check if other selection is synced
    g_assert_cmpstr(dummy_selection_paste(psel, "text/plain"), ==, "hello");
    g_assert_cmpstr(dummy_selection_paste(psel, "TEXT"), ==, "hello");
    g_assert_cmpstr(dummy_selection_paste(rsel, "text/plain"), ==, "hello");
    g_assert_cmpstr(dummy_selection_paste(rsel, "TEXT"), ==, "hello");

    // Check if selection that copied the text is not owned by us
    g_assert_false(clippor_selection_is_owned(CLIPPOR_SELECTION(rsel)));
}

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    g_test_init(&argc, &argv, NULL);

    test_setup();

    TEST("/clipboard/update", test_clipboard_update);

    return g_test_run();
}
