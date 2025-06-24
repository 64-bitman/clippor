#include "database.h"
#include "dbus-service.h"
#include "global.h"
#include "wayland-connection.h"
#include "wayland-seat.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <locale.h>
#include <stdlib.h>

static gboolean opt_version = FALSE;
static gboolean opt_server = FALSE;
static gboolean opt_debug = FALSE;

static GOptionEntry help_entries[] = {
    {"version", 'v', 0, G_OPTION_ARG_NONE, &opt_version, "Print version", NULL},
    {"server", 's', 0, G_OPTION_ARG_NONE, &opt_server, "Serve the clipboard",
     NULL},
    {"debug", 'd', 0, G_OPTION_ARG_NONE, &opt_debug, "Enable verbose logging",
     NULL},
    G_OPTION_ENTRY_NULL
};

static gboolean DONE = FALSE;

static gboolean on_sigint(gpointer user_data);
static void do_exit(void);
static void weak_ref_cb(gpointer data, GObject *object);
static void
allowed_mime_types_changed_cb(GObject *object, char *key, gpointer data);
static void
mime_type_groups_changed_cb(GObject *object, char *key, gpointer data);

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
        MAIN_CONTEXT = g_main_context_default();
        MAIN_LOOP = g_main_loop_new(MAIN_CONTEXT, FALSE);
        SETTINGS = g_settings_new("com.github.64bitman.clippor");

        g_signal_connect(
            SETTINGS, "changed::allowed-mime-types",
            G_CALLBACK(allowed_mime_types_changed_cb), NULL
        );
        g_signal_connect(
            SETTINGS, "changed::mime-type-groups",
            G_CALLBACK(mime_type_groups_changed_cb), NULL
        );
        allowed_mime_types_changed_cb(NULL, NULL, NULL);
        mime_type_groups_changed_cb(NULL, NULL, NULL);

        if (!database_init(&error))
        {
            g_assert(error != NULL);
            goto exit;
        }
        if (!dbus_service_init(&error))
        {
            g_assert(error != NULL);
            goto exit;
        }

        CLIPBOARDS = g_ptr_array_new_full(2, g_object_unref);
        CONNECTIONS = g_ptr_array_new_full(2, NULL);
        CLIENTS = g_ptr_array_new_full(2, NULL);

        // Get the default Wayland client using $WAYLAND_DISPLAY
        WaylandConnection *ct = wayland_connection_new(NULL, &error);

        if (ct == NULL)
        {
            g_assert(error != NULL);
            goto exit;
        }

        wayland_connection_install_source(ct, MAIN_CONTEXT);

        g_object_weak_ref(G_OBJECT(ct), weak_ref_cb, CONNECTIONS);
        g_ptr_array_add(CONNECTIONS, ct);

        // Create default clipboard
        ClipporClipboard *cb = clippor_clipboard_new("Default");

        g_ptr_array_add(CLIPBOARDS, cb);

        GHashTableIter iter;
        WaylandSeat *seat;

        g_hash_table_iter_init(&iter, wayland_connection_get_seats(ct));

        // Add seats to array
        while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&seat))
        {
            g_object_weak_ref(G_OBJECT(seat), weak_ref_cb, CLIENTS);
            g_ptr_array_add(CLIENTS, seat);

            clippor_clipboard_add_client(
                cb, wayland_seat_get_name(seat), CLIPPOR_CLIENT(seat),
                CLIPPOR_SELECTION_TYPE_REGULAR
            );
        }

        g_unix_signal_add(SIGINT, on_sigint, NULL);

        g_main_loop_run(MAIN_LOOP);

        do_exit();
    }

exit:
    if (error != NULL)
    {
        g_critical("%s\n", error->message);
        g_error_free(error);
        do_exit();
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static gboolean
on_sigint(gpointer user_data G_GNUC_UNUSED)
{
    do_exit();
    return G_SOURCE_REMOVE;
}

static void
do_exit(void)
{
    if (DONE)
        return;

    g_message("Exiting...");

    g_main_loop_quit(MAIN_LOOP);
    g_main_loop_unref(MAIN_LOOP);
    g_object_unref(SETTINGS);

    g_ptr_array_unref(CLIPBOARDS);
    g_ptr_array_unref(CONNECTIONS);
    g_ptr_array_unref(CLIENTS);

    g_ptr_array_unref(ALLOWED_MIME_TYPES);
    g_hash_table_unref(MIME_TYPE_GROUPS);

    g_object_unref(DBUS_SERVICE_OBJ_MANAGER);
    g_bus_unown_name(DBUS_SERVICE_IDENTIFIER);

    DONE = TRUE;
}

static void
weak_ref_cb(gpointer data, GObject *object)
{
    g_ptr_array_remove(data, object);
}

static void
allowed_mime_types_changed_cb(
    GObject *object G_GNUC_UNUSED, char *key G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    char **arr = g_settings_get_strv(SETTINGS, "allowed-mime-types");

    if (ALLOWED_MIME_TYPES != NULL)
        g_ptr_array_unref(ALLOWED_MIME_TYPES);

    ALLOWED_MIME_TYPES =
        g_ptr_array_new_with_free_func((void (*)(void *))g_regex_unref);

    char *pattern;
    int i = 0;

    while (pattern = arr[i++], pattern != NULL)
    {
        GError *error = NULL;
        GRegex *regex = g_regex_new(
            pattern, G_REGEX_OPTIMIZE, G_REGEX_MATCH_DEFAULT, &error
        );

        if (regex == NULL)
        {
            g_message(
                "allowed-mime-types: Failed compiling regex '%s': %s", pattern,
                error->message
            );
            g_error_free(error);
            continue;
        }
        g_ptr_array_add(ALLOWED_MIME_TYPES, regex);
    }
    g_strfreev(arr);
}

static void
mime_type_groups_changed_cb(
    GObject *object G_GNUC_UNUSED, char *key G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    GVariant *value = g_settings_get_value(SETTINGS, "mime-type-groups");

    if (MIME_TYPE_GROUPS != NULL)
        g_hash_table_unref(MIME_TYPE_GROUPS);

    MIME_TYPE_GROUPS = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, (void (*)(void *))g_regex_unref,
        (void (*)(void *))g_ptr_array_unref
    );

    // Loop through dictionary
    GVariantIter iter;
    GVariantIter *group_iter;
    char *pattern;

    g_variant_iter_init(&iter, value);

    while (g_variant_iter_next(&iter, "{sas}", &pattern, &group_iter))
    {
        // Compile regex
        GError *error = NULL;
        GRegex *regex = g_regex_new(
            pattern, G_REGEX_OPTIMIZE, G_REGEX_MATCH_DEFAULT, &error
        );

        if (regex == NULL)
        {
            g_message(
                "mime-type-groups: Failed compiling regex '%s': %s", pattern,
                error->message
            );
            g_error_free(error);
            continue;
        }

        // Get mime types in group
        GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
        char *mime_type;

        while (g_variant_iter_next(group_iter, "s", &mime_type))
            g_ptr_array_add(arr, mime_type);

        g_hash_table_insert(MIME_TYPE_GROUPS, regex, arr);

        g_variant_iter_free(group_iter);
        g_free(pattern);
    }

    g_variant_unref(value);
}
