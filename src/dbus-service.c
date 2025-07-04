#include "dbus-service.h"
#include "clippor-clipboard.h"
#include "com.github.clippor.h"
#include "database.h"
#include "global.h"
#include "util.h"
#include <gio-unix-2.0/gio/gunixoutputstream.h>
#include <gio/gio.h>

static void
bus_acquired_cb(
    GDBusConnection *connection, const char *name G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    DBUS_SERVICE_CONNECTION = connection;
    DBUS_SERVICE_OBJ_MANAGER =
        g_dbus_object_manager_server_new("/com/github/clippor");
}

static void
name_acquired_cb(
    GDBusConnection *connection G_GNUC_UNUSED, const char *name G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    g_main_loop_quit(MAIN_LOOP);
}

static void
name_lost_cb(
    GDBusConnection *connection, const char *name G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    if (connection == NULL)
        // Failed connecting to bus
        g_message("Failed connecting to session bus");
    else
        // Name cannot be obtained
        g_message("Failed obtaining name on session bus");

    g_main_loop_quit(MAIN_LOOP);
}

gboolean
dbus_service_init(GError **error)
{
    g_assert(error == NULL || *error == NULL);

    DBUS_SERVICE_IDENTIFIER = g_bus_own_name(
        G_BUS_TYPE_SESSION, "com.github.clippor", G_BUS_NAME_OWNER_FLAGS_NONE,
        bus_acquired_cb, name_acquired_cb, name_lost_cb, NULL, NULL
    );

    // Run main loop until we acquire name.
    g_main_loop_run(MAIN_LOOP);

    return TRUE;
}

static gboolean
get_entry_info_method_cb(
    BusClippor *object, GDBusMethodInvocation *invocation, int64_t index,
    gpointer data
)
{
    ClipporClipboard *cb = data;

    char *id;
    int64_t creation_time, last_used_time;
    uint sz;
    const char **mime_types;
    gboolean starred;

    GError *error = NULL;
    ClipporEntry *entry = clippor_clipboard_get_entry(cb, index, &error);

    if (entry == NULL)
    {
        g_assert(error != NULL);
        char *msg = g_strdup_printf(
            "Failed getting entry from database: %s", error->message
        );
        g_error_free(error);

        g_dbus_method_invocation_return_dbus_error(
            invocation,
            "com.github.clippor.ObjectManager.Error.FailedGettingEntry", msg
        );
        g_free(msg);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

    g_object_get(
        entry, "id", &id, "creation-time", &creation_time, "last-used-time",
        &last_used_time, "starred", &starred, NULL
    );
    mime_types = (const char **)g_hash_table_get_keys_as_array(
        clippor_entry_get_mime_types(entry), &sz
    );

    // TODO: allow for multiple clippor instances
    bus_clippor_complete_get_entry_info(
        object, invocation, id, creation_time, last_used_time, starred,
        mime_types
    );

    g_free(id);
    g_free(mime_types);
    g_object_unref(entry);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
get_mime_type_data_method_cb(
    BusClippor *object, GDBusMethodInvocation *invocation, const char *id,
    const char *mime_type, GVariant *fd_variant, gpointer user_data
)
{
    ClipporClipboard *cb = user_data;
    int32_t fd_index = g_variant_get_handle(fd_variant);
    GDBusMessage *message = g_dbus_method_invocation_get_message(invocation);
    GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(message);

    if (g_unix_fd_list_get_length(fd_list) == 0)
        return G_DBUS_METHOD_INVOCATION_UNHANDLED;

    char *err_msg, *err_suffix;
    GError *error = NULL;
    int fd = g_unix_fd_list_get(fd_list, fd_index, &error);

    if (fd == -1)
    {
        err_msg = "Failed getting file descriptor from list";
        err_suffix = "FailedGettingFd";
        goto fail;
    }

    ClipporEntry *entry = clippor_clipboard_get_entry_by_id(cb, id, &error);

    if (entry == NULL)
    {
        err_msg = "Failed getting entry";
        err_suffix = "FailedGettingEntry";
        goto fail;
    }

    GBytes *data = clippor_entry_get_data(entry, mime_type, &error);

    if (data == NULL)
    {
        err_msg = "Failed getting entry data for mime type";
        err_suffix = "FailedGettingMimeTypeData";
        goto fail;
    }

    if (!util_send_data(
            fd, data, g_settings_get_int(SETTINGS, "data-timeout"), &error
        ))
    {
        err_msg = "Failed sending data";
        err_suffix = "FailedSendingData";
        goto fail;
    }

    g_bytes_unref(data);
    g_object_unref(entry);

    bus_clippor_complete_get_mime_type_data(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
fail:
    g_assert(error != NULL);

    char *msg = g_strdup_printf("%s: %s", err_msg, error->message);
    char *err_name = g_strdup_printf(
        "com.github.clippor.ObjectManager.Error.%s", err_suffix
    );

    g_error_free(error);

    g_dbus_method_invocation_return_dbus_error(invocation, err_name, msg);
    g_free(msg);
    g_free(err_name);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
get_entries_count_method_cb(
    BusClippor *object, GDBusMethodInvocation *invocation, gpointer user_data
)
{
    ClipporClipboard *cb = user_data;

    GError *error = NULL;
    int64_t num = database_get_num_entries(cb, &error);

    if (num == -1)
    {
        g_assert(error != NULL);
        char *msg = g_strdup_printf(
            "Failed getting entry count from database: %s", error->message
        );
        g_error_free(error);

        g_dbus_method_invocation_return_dbus_error(
            invocation,
            "com.github.clippor.ObjectManager.Error.FailedGettingEntryCount",
            msg
        );
        g_free(msg);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

    bus_clippor_complete_get_entries_count(object, invocation, num);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

void
dbus_service_add_clipboard(ClipporClipboard *cb)
{
    g_assert(cb != NULL);
    char *path = g_strdup_printf(
        "/com/github/clippor/%s", clippor_clipboard_get_label(cb)
    );
    BusObjectSkeleton *object = bus_object_skeleton_new(path);

    g_free(path);

    // Export interface for object
    BusClippor *iface = bus_clippor_skeleton_new();

    bus_object_skeleton_set_clippor(object, iface);

    g_signal_connect(
        iface, "handle-get-entry-info", G_CALLBACK(get_entry_info_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-get-mime-type-data",
        G_CALLBACK(get_mime_type_data_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-get-entries-count",
        G_CALLBACK(get_entries_count_method_cb), cb
    );

    g_dbus_object_manager_server_export(
        DBUS_SERVICE_OBJ_MANAGER, G_DBUS_OBJECT_SKELETON(object)
    );
    g_object_unref(object);

    g_dbus_object_manager_server_set_connection(
        DBUS_SERVICE_OBJ_MANAGER, DBUS_SERVICE_CONNECTION
    );
}
