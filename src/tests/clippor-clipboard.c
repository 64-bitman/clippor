#include "test_util.h"
#include <glib.h>
#include <locale.h>

static void
test_clippor_clipboard_add_client(void)
{
    TesterClient *client = g_object_new(TESTER_TYPE_CLIENT, NULL);
    ClipporClipboard *cb = clippor_clipboard_new("Test");

    clippor_clipboard_add_client(
        cb, "client", CLIPPOR_CLIENT(client), CLIPPOR_SELECTION_TYPE_REGULAR
    );

    TesterClientSelection *sel =
        tester_client_get_selection(client, CLIPPOR_SELECTION_TYPE_REGULAR);

    g_ptr_array_add(sel->mime_types, g_strdup("text/plain"));

    sel->data = g_bytes_new_static("data", 4);

    g_signal_emit_by_name(client, "selection", CLIPPOR_SELECTION_TYPE_REGULAR);

    ClipporEntry *entry = clippor_clipboard_get_entry(cb, 0);

    g_assert_nonnull(entry);

    g_object_unref(client);
    g_object_unref(cb);
}

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/clipboard/new", test_clippor_clipboard_add_client);

    return g_test_run();
}
