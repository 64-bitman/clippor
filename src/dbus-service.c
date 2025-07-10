#include "dbus-service.h"
#include "clippor-clipboard.h"
#include "com.github.clippor.h"
#include "database.h"
#include "glib-unix.h"
#include "server.h"
#include "util.h"
#include <gio-unix-2.0/gio/gunixoutputstream.h>
#include <gio/gio.h>

gboolean DBUS_READY = FALSE;
static GDBusConnection *DBUS_SERVICE_CONNECTION;
static uint DBUS_SERVICE_IDENTIFIER;
static GDBusObjectManagerServer *DBUS_SERVICE_OBJ_MANAGER;

static int TIMEOUT;

static void
bus_acquired_cb(
    GDBusConnection *connection, const char *name G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    DBUS_SERVICE_CONNECTION = connection;
    DBUS_SERVICE_OBJ_MANAGER = g_dbus_object_manager_server_new("/com/github");
}

static void
name_acquired_cb(
    GDBusConnection *connection G_GNUC_UNUSED, const char *name G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    g_main_loop_quit(server_get_main_loop());
}

static void
name_lost_cb(
    GDBusConnection *connection, const char *name G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    if (connection == NULL)
        // Failed connecting to bus
        g_warning("Failed connecting to session bus");
    else
        // Name cannot be obtained
        g_warning("Failed obtaining name on session bus");

    g_main_loop_quit(server_get_main_loop());
}

static gboolean
list_clipboards_method_cb(
    BusClippor *object, GDBusMethodInvocation *invocation,
    gpointer user_data G_GNUC_UNUSED
)
{
    const GPtrArray *cbs = server_get_clipboards();
    const char **names = g_malloc0_n(cbs->len + 1, sizeof(char *));

    for (uint i = 0; i < cbs->len; i++)
        names[i] = clippor_clipboard_get_label(cbs->pdata[i]);

    bus_clippor_complete_list_clipboards(object, invocation, names);

    g_free(names);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

gboolean
dbus_service_init(GError **error, int timeout)
{
    g_assert(error == NULL || *error == NULL);

    DBUS_SERVICE_IDENTIFIER = g_bus_own_name(
        G_BUS_TYPE_SESSION, "com.github.clippor", G_BUS_NAME_OWNER_FLAGS_NONE,
        bus_acquired_cb, name_acquired_cb, name_lost_cb, NULL, NULL
    );

    TIMEOUT = timeout;

    // Run main loop until we acquire name.
    g_main_loop_run(server_get_main_loop());

    // Create central interface and object
    BusObjectSkeleton *object = bus_object_skeleton_new("/com/github/clippor");
    BusClippor *iface = bus_clippor_skeleton_new();

    bus_object_skeleton_set_clippor(object, iface);
    g_object_unref(iface);

    g_signal_connect(
        iface, "handle-list-clipboards", G_CALLBACK(list_clipboards_method_cb),
        NULL
    );

    g_dbus_object_manager_server_export(
        DBUS_SERVICE_OBJ_MANAGER, G_DBUS_OBJECT_SKELETON(object)
    );
    g_object_unref(object);

    g_dbus_object_manager_server_set_connection(
        DBUS_SERVICE_OBJ_MANAGER, DBUS_SERVICE_CONNECTION
    );

    DBUS_READY = TRUE;

    return TRUE;
}

void
dbus_server_uninit(void)
{
    if (DBUS_READY)
    {
        g_object_unref(DBUS_SERVICE_OBJ_MANAGER);
        g_bus_unown_name(DBUS_SERVICE_IDENTIFIER);
        DBUS_READY = FALSE;
    }
}

#define DBUS_ERROR(errname, errmsg)                                            \
    do                                                                         \
    {                                                                          \
        g_dbus_method_invocation_return_dbus_error(                            \
            invocation, "com.github.clippor.ObjectManager.Error." errname,     \
            errmsg                                                             \
        );                                                                     \
        return G_DBUS_METHOD_INVOCATION_HANDLED;                               \
    } while (FALSE)

#define DBUS_ERRORE(errname, errmsg)                                           \
    do                                                                         \
    {                                                                          \
        g_assert(error != NULL);                                               \
        char *msg = g_strdup_printf(errmsg ": %s", error->message);            \
        g_error_free(error);                                                   \
        g_dbus_method_invocation_return_dbus_error(                            \
            invocation, "com.github.clippor.ObjectManager.Error." errname, msg \
        );                                                                     \
        g_free(msg);                                                           \
        return G_DBUS_METHOD_INVOCATION_HANDLED;                               \
    } while (FALSE)

static gboolean
get_entry_info_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    int64_t index, gpointer data
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
        DBUS_ERRORE("FailedGettingEntry", "Failed getting entry from database");

    g_object_get(
        entry, "id", &id, "creation-time", &creation_time, "last-used-time",
        &last_used_time, "starred", &starred, NULL
    );
    mime_types = (const char **)g_hash_table_get_keys_as_array(
        clippor_entry_get_mime_types(entry), &sz
    );

    bus_clippor_clipboard_complete_get_entry_info(
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
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    const char *id, const char *mime_type, GVariant *fd_variant,
    gpointer user_data
)
{
    ClipporClipboard *cb = user_data;
    int32_t fd_index = g_variant_get_handle(fd_variant);
    GDBusMessage *message = g_dbus_method_invocation_get_message(invocation);
    GUnixFDList *fd_list = g_dbus_message_get_unix_fd_list(message);

    if (g_unix_fd_list_get_length(fd_list) == 0)
        DBUS_ERROR("EmptyFDList", "FD list is empty");

    GError *error = NULL;
    int fd = g_unix_fd_list_get(fd_list, fd_index, &error);

    if (fd == -1)
        DBUS_ERRORE(
            "FailedGettingFD", "Failed getting file descriptor from list"
        );

    ClipporEntry *entry = clippor_clipboard_get_entry_by_id(cb, id, &error);

    if (entry == NULL)
        DBUS_ERRORE("FailedGettingEntry", "Failed getting entry");

    ClipporData *data = clippor_entry_get_data(entry, mime_type, &error);

    if (data == NULL)
    {
        if (error != NULL)
            DBUS_ERRORE(
                "FailedGettingMimeTypeData", "Failed getting data for mime type"
            );
        else
            DBUS_ERROR("NoMimeTypeData", "No data associated with mime type");
    }

    if (!util_send_data(fd, data, TIMEOUT, &error))
        DBUS_ERRORE("FailedSendingData", "Failed sending data");

    clippor_data_unref(data);
    g_object_unref(entry);

    bus_clippor_clipboard_complete_get_mime_type_data(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
get_entries_count_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    gpointer user_data
)
{
    ClipporClipboard *cb = user_data;

    GError *error = NULL;
    int64_t num = database_get_num_entries(cb, &error);

    if (num == -1)
        DBUS_ERRORE(
            "FailedGettingEntryCount",
            "Failed getting entry count from database"
        );

    bus_clippor_clipboard_complete_get_entries_count(object, invocation, num);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
remove_entry_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    const char *id, gpointer user_data
)
{
    ClipporClipboard *cb = user_data;
    GError *error = NULL;

    if (!clippor_clipboard_remove_entry(cb, id, &error))
        DBUS_ERRORE("FailedRemovingEntry", "Failed removing entry");

    bus_clippor_clipboard_complete_remove_entry(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
entry_exists_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    const char *id, gpointer user_data G_GNUC_UNUSED
)
{
    GError *error = NULL;
    int ret = database_entry_id_exists(id, &error);

    if (ret == -1)
        DBUS_ERRORE(
            "FailedCheckingEntry", "Failed checking if entry id exists"
        );

    bus_clippor_clipboard_complete_entry_exists(object, invocation, ret == 0);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
clear_history_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    gpointer user_data
)
{
    ClipporClipboard *cb = user_data;
    GError *error = NULL;

    if (!clippor_clipboard_clear_history(cb, &error))
        DBUS_ERRORE("FailedClearingHistory", "Failed clearing history");

    bus_clippor_clipboard_complete_clear_history(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
set_entry_data_method_cb(
    BusClipporClipboard *object G_GNUC_UNUSED,
    GDBusMethodInvocation *invocation, const char *id, const char **mime_types,
    gpointer user_data
)
{
    ClipporClipboard *cb = user_data;
    GError *error = NULL;

    // Check if entry exists
    int ret = database_entry_id_exists(id, &error);

    if (ret == -1)
        DBUS_ERRORE(
            "FailedCheckingEntry", "Failed checking if entry id exists"
        );
    else if (ret == 1)
        DBUS_ERROR("EntryDoesNotExist", "Entry with id does not exist");

    // Create pipe to receive data from
    g_auto(GUnixPipe) fds = G_UNIX_PIPE_INIT;

    if (!g_unix_pipe_open(&fds, O_CLOEXEC, &error))
        DBUS_ERRORE("FailedCreatingPipe", "Failed creating pipe");

    // Create a unix fd list to send the write fd to the client
    g_autoptr(GUnixFDList) fd_list = g_unix_fd_list_new();
    int fd_index = g_unix_fd_list_append(
        fd_list, g_unix_pipe_get(&fds, G_UNIX_PIPE_END_WRITE), &error
    );

    if (fd_index == -1)
        DBUS_ERRORE("FailedAppendingFD", "Failed appending FD to list");

    g_dbus_method_invocation_return_value_with_unix_fd_list(
        invocation, g_variant_new("(h)", fd_index), fd_list
    );

    // Close so we receive EOF
    g_unix_pipe_close(&fds, G_UNIX_PIPE_END_WRITE, NULL);

    // Receive data
    g_autoptr(ClipporData) data = util_receive_data(
        g_unix_pipe_get(&fds, G_UNIX_PIPE_END_READ), TIMEOUT, TRUE, &error
    );

    if (data == NULL)
    {
        g_warning("SetEntryData(): Failed receiving data: %s", error->message);
        g_error_free(error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

    // Update history and database
    g_autoptr(ClipporEntry) entry =
        clippor_clipboard_get_entry_by_id(cb, id, &error);

    if (entry == NULL)
    {
        g_warning(
            "SetEntryData(): Failed getting entry by id: %s", error->message
        );
        g_error_free(error);
        return G_DBUS_METHOD_INVOCATION_HANDLED;
    }

    for (int i = 0; mime_types[i] != NULL; i++)
        if (!clippor_entry_set_mime_type(entry, mime_types[i], data, &error))
        {
            g_warning(
                "SetEntryData(): Failed setting mime_type_data: %s",
                error->message
            );
            g_clear_error(&error);
        }

    clippor_clipboard_update_clients(cb, entry, FALSE);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
remove_entry_data_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    const char *id, const char **mime_types, gpointer user_data
)
{
    ClipporClipboard *cb = user_data;
    GError *error = NULL;

    g_autoptr(ClipporEntry) entry =
        clippor_clipboard_get_entry_by_id(cb, id, &error);

    if (entry == NULL)
        DBUS_ERRORE("FailedGettingEntry", "Failed getting entry by id");

    for (int i = 0; mime_types[i] != NULL; i++)
        if (!clippor_entry_set_mime_type(entry, mime_types[i], NULL, &error))
            DBUS_ERRORE(
                "FailedSettingMimeTypeData", "Failed settings mime type data"
            );

    clippor_clipboard_update_clients(cb, entry, FALSE);

    bus_clippor_clipboard_complete_remove_entry_data(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
new_entry_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    gpointer user_data
)
{
    ClipporClipboard *cb = user_data;

    GError *error = NULL;
    ClipporEntry *entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );

    if (entry == NULL)
        DBUS_ERRORE("FailedCreatingEntry", "Failed creating entry");

    clippor_clipboard_add_entry(cb, entry);

    bus_clippor_clipboard_complete_new_entry(
        object, invocation, clippor_entry_get_id(entry)
    );

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
set_entry_starred_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    const char *id, gboolean starred, gpointer user_data
)
{
    ClipporClipboard *cb = user_data;

    GError *error = NULL;
    ClipporEntry *entry = clippor_clipboard_get_entry_by_id(cb, id, &error);

    if (entry == NULL)
        DBUS_ERRORE("FailedGettingEntry", "Failed getting entry by id");

    if (!clippor_entry_update_property(entry, &error, "starred", starred, NULL))
    {
        g_object_unref(entry);
        DBUS_ERRORE("FailedUpdatingEntry", "Failed updating entry");
    }

    g_object_unref(entry);

    bus_clippor_clipboard_complete_set_entry_starred(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
update_entry_last_used_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    const char *id, gpointer user_data
)
{
    ClipporClipboard *cb = user_data;

    GError *error = NULL;
    ClipporEntry *entry = clippor_clipboard_get_entry_by_id(cb, id, &error);

    if (entry == NULL)
        DBUS_ERRORE("FailedGettingEntry", "Failed getting entry by id");

    if (!clippor_entry_update_last_used(entry, &error))
    {
        g_object_unref(entry);
        DBUS_ERRORE("FailedUpdatingEntry", "Failed updating entry");
    }

    g_object_unref(entry);

    bus_clippor_clipboard_complete_update_entry_last_used(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
list_entries_starred_status_method_cb(
    BusClipporClipboard *object, GDBusMethodInvocation *invocation,
    gboolean starred, gpointer user_data G_GNUC_UNUSED
)
{
    GError *error = NULL;
    GPtrArray *array = database_list_entries_starred_status(starred, &error);

    if (array == NULL)
        DBUS_ERRORE(
            "FailedGettingEntries", "Failed getting starred/non-starred entries"
        );

    bus_clippor_clipboard_complete_list_entries_starred_status(
        object, invocation, (const char **)array->pdata
    );

    g_ptr_array_unref(array);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

void
dbus_service_add_clipboard(ClipporClipboard *cb)
{
    g_assert(cb != NULL);

    if (!DBUS_READY)
        return;

    char *path = g_strdup_printf(
        "/com/github/clippor/clipboards/%s", clippor_clipboard_get_label(cb)
    );
    BusObjectSkeleton *object = bus_object_skeleton_new(path);

    g_free(path);

    // Export interface for object
    BusClipporClipboard *iface = bus_clippor_clipboard_skeleton_new();

    bus_object_skeleton_set_clippor_clipboard(object, iface);
    g_object_unref(iface);

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
    g_signal_connect(
        iface, "handle-remove-entry", G_CALLBACK(remove_entry_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-entry-exists", G_CALLBACK(entry_exists_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-clear-history", G_CALLBACK(clear_history_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-set-entry-data", G_CALLBACK(set_entry_data_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-remove-entry-data",
        G_CALLBACK(remove_entry_data_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-new-entry", G_CALLBACK(new_entry_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-set-entry-starred",
        G_CALLBACK(set_entry_starred_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-update-entry-last-used",
        G_CALLBACK(update_entry_last_used_method_cb), cb
    );
    g_signal_connect(
        iface, "handle-list-entries-starred-status",
        G_CALLBACK(list_entries_starred_status_method_cb), cb
    );

    g_dbus_object_manager_server_export(
        DBUS_SERVICE_OBJ_MANAGER, G_DBUS_OBJECT_SKELETON(object)
    );
    g_object_unref(object);

    g_dbus_object_manager_server_set_connection(
        DBUS_SERVICE_OBJ_MANAGER, DBUS_SERVICE_CONNECTION
    );
}
