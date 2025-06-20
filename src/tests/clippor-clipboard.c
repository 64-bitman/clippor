#include <glib.h>
#include <locale.h>

static void
test_clippor_clipboard_add_client(void)
{
}


int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/clipboard/new", test_clippor_clipboard_add_client);

    return g_test_run();
}
