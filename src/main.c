#include "server.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <locale.h>
#include <stdlib.h>

static gboolean opt_version = FALSE;
static gboolean opt_server = FALSE;
static gboolean opt_debug = FALSE;
static char *opt_config = NULL;
static char *opt_data = NULL;

static GOptionEntry help_entries[] = {
    {"version", 'v', 0, G_OPTION_ARG_NONE, &opt_version, "Print version", NULL},
    {"server", 's', 0, G_OPTION_ARG_NONE, &opt_server, "Serve the clipboard",
     NULL},
    {"debug", 'd', 0, G_OPTION_ARG_NONE, &opt_debug, "Enable verbose logging",
     NULL},
    {"config", 'c', 0, G_OPTION_ARG_STRING, &opt_config, "Config file location",
     NULL},
    {"data", 'D', 0, G_OPTION_ARG_STRING, &opt_data, "Data directory location",
     NULL},
    G_OPTION_ENTRY_NULL
};

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    GError *error = NULL;
    GOptionContext *context = g_option_context_new("- clipboard manager");

    // Read commandline
    g_option_context_add_main_entries(context, help_entries, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_option_context_free(context);
        goto exit;
    }
    g_option_context_free(context);

    // Serve some command line flags
    if (opt_version)
    {
        g_print("Clipstuff version " VERSION "\n");
        goto exit;
    }
    if (opt_debug)
        g_log_set_debug_enabled(TRUE);

    if (opt_server)
    {
        if (!server_start(opt_config, opt_data))
            goto exit;
    }

    g_free(opt_config);

exit:
    if (error != NULL)
    {
        g_warning("%s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
