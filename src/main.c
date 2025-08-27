#include "clippor-server.h"
#include "modules.h"
#include <glib.h>

static gboolean opt_version;
static gboolean opt_debug;

static char *opt_config_file;
static char *opt_data_dir;

static GOptionEntry entries[] = {
    {"version", 'v', 0, G_OPTION_ARG_NONE, &opt_version, "Show version", NULL},
    {"debug", 'd', 0, G_OPTION_ARG_NONE, &opt_debug, "Be more verbose", NULL},
    {"config-file", 'c', 0, G_OPTION_ARG_STRING, &opt_config_file,
     "Configuration file to use", NULL},
    {"data-dir", 'D', 0, G_OPTION_ARG_STRING, &opt_data_dir,
     "Data directory to use", NULL},
    G_OPTION_ENTRY_NULL
};

int
main(int argc, char **argv)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GOptionContext) context =
        g_option_context_new(" - clipboard manager");

    g_option_context_add_main_entries(context, entries, NULL);

    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_print("Option parsing failed: %s\n", error->message);
        return EXIT_FAILURE;
    }

    if (opt_version)
    {
        g_print("Clippor version " VERSION "\n");
        return EXIT_SUCCESS;
    }

    if (opt_debug)
        g_log_set_debug_enabled(TRUE);

    modules_init();

    g_autoptr(ClipporConfig) cfg;
    g_autoptr(ClipporDatabase) db;

    cfg = clippor_config_new_file(opt_config_file, &error);

    if (cfg == NULL)
    {
        g_warning("%s", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }

    db = clippor_database_new(opt_data_dir, CLIPPOR_DATABASE_DEFAULT, &error);

    if (db == NULL)
    {
        g_warning("%s", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }

    g_autoptr(ClipporServer) server = clippor_server_new(cfg, db);

    if (!clippor_server_start(server, &error))
    {
        g_warning("%s", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }

    g_free(opt_config_file);
    g_free(opt_data_dir);

    // Make sure this is always called last to avoid bugs
    modules_uninit();

    return EXIT_SUCCESS;
}
