#include "server.h"
#include <glib.h>

static gboolean opt_version;
static gboolean opt_debug;
static gboolean opt_server;

static char *opt_config_file;
static char *opt_data_dir;

static GOptionEntry entries[] = {
    {"version", 'v', 0, G_OPTION_ARG_NONE, &opt_version, "Show version", NULL},
    {"debug", 'd', 0, G_OPTION_ARG_NONE, &opt_debug, "Be more verbose", NULL},
    {"server", 's', 0, G_OPTION_ARG_NONE, &opt_server, "Start the server",
     NULL},
    {"config-file", 'c', 0, G_OPTION_ARG_STRING, &opt_config_file,
     "Configuration file to use", NULL},
    {"data-dir", 'D', 0, G_OPTION_ARG_STRING, &opt_data_dir,
     "Data directory to use", NULL},
    G_OPTION_ENTRY_NULL
};

int
main(int argc, char **argv)
{
    GError *error = NULL;
    GOptionContext *context = g_option_context_new(" - clipboard manager");

    g_option_context_add_main_entries(context, entries, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_print("Option parsing failed: %s\n", error->message);
        return EXIT_FAILURE;
    }
    g_option_context_free(context);

    if (opt_version)
    {
        g_print("Clippor version " VERSION);
        return EXIT_SUCCESS;
    }

    if (opt_debug)
        g_log_set_debug_enabled(TRUE);

    int ret = EXIT_SUCCESS;

    if (opt_server)
        if (!server_start(opt_config_file, opt_data_dir))
            ret = EXIT_FAILURE;

    g_free(opt_config_file);
    g_free(opt_data_dir);

    return ret;
}
