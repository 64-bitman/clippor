#include "wayland-seat.h"
#include "dbus-service.h"
#include "clippor-clipboard.h"
#include "com.github.clippor.h"
#include "server.h"
#include "util.h"
#include "wayland-connection.h"
#include <gio/gio.h>
#include <glib-unix.h>

static GDBusConnection *DBUS_SERVICE_CONNECTION;
static uint DBUS_SERVICE_IDENTIFIER;

static GDBusObjectManagerServer *CLIPBOARD_OBJ_MANAGER;
static GDBusObjectManagerServer *WAYLAND_CT_OBJ_MANAGER;
static GDBusObjectManagerServer *PRIMARY_OBJ_MANAGER;

static gboolean DBUS_READY = FALSE;
static gboolean DBUS_DESTROYED = FALSE;
static int TIMEOUT;

static void primary_interface_init(void);

static void
bus_acquired_cb(
    GDBusConnection *connection, const char *name G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    DBUS_SERVICE_CONNECTION = connection;
    CLIPBOARD_OBJ_MANAGER =
        g_dbus_object_manager_server_new("/com/github/Clippor/Clipboards");
    WAYLAND_CT_OBJ_MANAGER = g_dbus_object_manager_server_new(
        "/com/github/Clippor/WaylandConnections"
    );
    PRIMARY_OBJ_MANAGER = g_dbus_object_manager_server_new("/com/github");

    // Start exporting objects
    g_dbus_object_manager_server_set_connection(
        CLIPBOARD_OBJ_MANAGER, connection
    );
    g_dbus_object_manager_server_set_connection(
        WAYLAND_CT_OBJ_MANAGER, connection
    );
    g_dbus_object_manager_server_set_connection(
        PRIMARY_OBJ_MANAGER, connection
    );
}

static void
name_acquired_cb(
    GDBusConnection *connection G_GNUC_UNUSED, const char *name G_GNUC_UNUSED,
    gpointer data G_GNUC_UNUSED
)
{
    if (!DBUS_READY)
        g_main_loop_quit(data);
}

static void
name_lost_cb(
    GDBusConnection *connection, const char *name G_GNUC_UNUSED, gpointer data
)
{
    if (connection == NULL)
        // Failed connecting to bus
        g_warning("Failed connecting to session bus");
    else
        // Name cannot be obtained
        g_warning("Failed obtaining name on session bus");

    if (!DBUS_READY)
        g_main_loop_quit(data);
    DBUS_READY = FALSE;
}

static void
bus_name_destroy_cb(gpointer user_data G_GNUC_UNUSED)
{
    DBUS_DESTROYED = TRUE;
}

gboolean
dbus_service_init(int timeout, GError **error)
{
    g_assert(error == NULL || *error == NULL);

    GMainLoop *loop =
        g_main_loop_new(g_main_context_get_thread_default(), FALSE);

    TIMEOUT = timeout;
    DBUS_SERVICE_IDENTIFIER = g_bus_own_name(
        G_BUS_TYPE_SESSION, "com.github.Clippor", G_BUS_NAME_OWNER_FLAGS_NONE,
        bus_acquired_cb, name_acquired_cb, name_lost_cb, loop,
        bus_name_destroy_cb
    );

    // Run main loop until we acquire name.
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    // Create primary object and interface
    primary_interface_init();

    DBUS_READY = TRUE;
    DBUS_DESTROYED = FALSE;

    return TRUE;
}

void
dbus_server_uninit(void)
{
    if (DBUS_READY)
    {
        g_object_unref(CLIPBOARD_OBJ_MANAGER);
        g_object_unref(WAYLAND_CT_OBJ_MANAGER);
        g_object_unref(PRIMARY_OBJ_MANAGER);

        g_bus_unown_name(DBUS_SERVICE_IDENTIFIER);

        // Iterate main context until there is no more DBus traffic queued up
        while (!DBUS_DESTROYED)
            g_main_context_iteration(g_main_context_get_thread_default(), TRUE);

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
clippor_list_clipboards_method_cb(
    BusClippor *object, GDBusMethodInvocation *invocation,
    gpointer user_data G_GNUC_UNUSED
)
{
    GHashTable *cbs = server_get_clipboards();

    // No key should be NULL
    const char **names =
        (const char **)g_hash_table_get_keys_as_array(cbs, NULL);

    bus_clippor_complete_list_clipboards(object, invocation, names);

    g_free(names);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
clippor_add_clipboard_method_cb(
    BusClippor *object, GDBusMethodInvocation *invocation, const char *label,
    gpointer user_data G_GNUC_UNUSED
)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new(label);

    if (!server_add_clipboard(cb, &error))
        DBUS_ERRORE("FailedAddingClipboard", "Failed adding clipboard");

    bus_clippor_complete_add_clipboard(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
clippor_list_wayland_connections_method_cb(
    BusClippor *object, GDBusMethodInvocation *invocation,
    gpointer user_data G_GNUC_UNUSED
)
{
    GHashTable *cts = server_get_wayland_connections();

    const char **displays =
        (const char **)g_hash_table_get_keys_as_array(cts, NULL);

    bus_clippor_complete_list_wayland_connections(object, invocation, displays);

    g_free(displays);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
clippor_add_wayland_connection_method_cb(
    BusClippor *object, GDBusMethodInvocation *invocation, const char *display,
    gpointer user_data G_GNUC_UNUSED
)
{
    GError *error = NULL;
    WaylandConnection *ct = wayland_connection_new(display, &error);

    if (!server_add_wayland_connection(ct, &error))
        DBUS_ERRORE(
            "FailedAddingWaylandConnection", "Failed adding Wayland connection"
        );

    bus_clippor_complete_add_wayland_connection(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
primary_interface_init(void)
{
    // Create primary object and interface
    BusObjectSkeleton *object = bus_object_skeleton_new("/com/github/Clippor");

    g_assert(object != NULL);

    BusClippor *iface = bus_clippor_skeleton_new();

    bus_object_skeleton_set_clippor(object, iface);
    g_object_unref(iface);

    g_signal_connect(
        iface, "handle-list-clipboards",
        G_CALLBACK(clippor_list_clipboards_method_cb), NULL
    );
    g_signal_connect(
        iface, "handle-add-clipboard",
        G_CALLBACK(clippor_add_clipboard_method_cb), NULL
    );
    g_signal_connect(
        iface, "handle-list-wayland-connections",
        G_CALLBACK(clippor_list_wayland_connections_method_cb), NULL
    );
    g_signal_connect(
        iface, "handle-add-wayland-connection",
        G_CALLBACK(clippor_add_wayland_connection_method_cb), NULL
    );

    g_dbus_object_manager_server_export(
        PRIMARY_OBJ_MANAGER, G_DBUS_OBJECT_SKELETON(object)
    );
    g_object_unref(object);
}

static gboolean
wayland_connections_list_seats_method_cb(
    BusClipporWaylandConnection *object, GDBusMethodInvocation *invocation,
    gpointer user_data
)
{
    WaylandConnection *ct = user_data;

    GHashTable *seats = wayland_connection_get_seats(ct);
    g_autofree char **names =
        (char **)g_hash_table_get_keys_as_array(seats, NULL);

    bus_clippor_wayland_connection_complete_list_seats(
        object, invocation, (const char **)names
    );

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
wayland_connections_connect_seat_method_cb(
    BusClipporWaylandConnection *object, GDBusMethodInvocation *invocation,
    const char *seat_name, const char *clipboard_name, uint selection,
    gpointer user_data
)
{
    WaylandConnection *ct = user_data;

    ClipporClipboard *cb = server_get_clipboard(clipboard_name);
    WaylandSeat *seat = wayland_connection_get_seat(ct, seat_name);

    if (cb == NULL)
        DBUS_ERROR("NoClipboard", "No such clipboard");
    if (seat == NULL)
        DBUS_ERROR("NoWaylandSeat", "No such Wayland seat");

    g_autofree char *label = g_strdup_printf(
        "%s_%s", wayland_connection_get_display_name(ct), seat_name
    );

    if (selection == 1)
        clippor_clipboard_add_client(
            cb, label, CLIPPOR_CLIENT(seat), CLIPPOR_SELECTION_TYPE_REGULAR
        );
    else if (selection == 2)
        clippor_clipboard_add_client(
            cb, label, CLIPPOR_CLIENT(seat), CLIPPOR_SELECTION_TYPE_PRIMARY
        );
    else
        DBUS_ERROR("UnknownSelection", "Unknown selection");

    bus_clippor_wayland_connection_complete_connect_seat(object, invocation);

    return G_DBUS_METHOD_INVOCATION_HANDLED;
}

void
dbus_service_add_wayland_connection(WaylandConnection *ct)
{
    g_assert(WAYLAND_IS_CONNECTION(ct));

    if (!DBUS_READY)
        return;

    g_autofree char *path = replace_dbus_illegal_chars(
        wayland_connection_get_display_name(ct),
        "/com/github/Clippor/WaylandConnections"
    );

    BusObjectSkeleton *object = bus_object_skeleton_new(path);

    g_assert(object != NULL);

    BusClipporWaylandConnection *iface =
        bus_clippor_wayland_connection_skeleton_new();

    bus_object_skeleton_set_clippor_wayland_connection(object, iface);
    g_object_unref(iface);

    g_signal_connect_object(
        iface, "handle-list-seats",
        G_CALLBACK(wayland_connections_list_seats_method_cb), ct,
        G_CONNECT_DEFAULT
    );
    g_signal_connect_object(
        iface, "handle-connect-seat",
        G_CALLBACK(wayland_connections_connect_seat_method_cb), ct,
        G_CONNECT_DEFAULT
    );

    g_dbus_object_manager_server_export(
        WAYLAND_CT_OBJ_MANAGER, G_DBUS_OBJECT_SKELETON(object)
    );
    g_object_unref(object);
}

void
dbus_service_remove_wayland_connection(WaylandConnection *ct)
{
    g_assert(WAYLAND_IS_CONNECTION(ct));

    if (!DBUS_READY)
        return;

    g_autofree char *path = replace_dbus_illegal_chars(
        wayland_connection_get_display_name(ct),
        "/com/github/Clippor/WaylandConnections"
    );

    g_dbus_object_manager_server_unexport(WAYLAND_CT_OBJ_MANAGER, path);
}

void
dbus_service_add_clipboard(ClipporClipboard *cb)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));

    if (!DBUS_READY)
        return;

    g_autofree char *path = replace_dbus_illegal_chars(
        clippor_clipboard_get_label(cb), "/com/github/Clippor/Clipboards"
    );

    BusObjectSkeleton *object = bus_object_skeleton_new(path);

    g_assert(object != NULL);

    // Export interface for object
    BusClipporClipboard *iface = bus_clippor_clipboard_skeleton_new();

    bus_object_skeleton_set_clippor_clipboard(object, iface);
    g_object_unref(iface);

    g_dbus_object_manager_server_export(
        CLIPBOARD_OBJ_MANAGER, G_DBUS_OBJECT_SKELETON(object)
    );
    g_object_unref(object);
}

void
dbus_service_remove_clipboard(ClipporClipboard *cb)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));

    if (!DBUS_READY)
        return;

    g_autofree char *path = replace_dbus_illegal_chars(
        clippor_clipboard_get_label(cb), "/com/github/Clippor/Clipboards"
    );

    g_dbus_object_manager_server_unexport(CLIPBOARD_OBJ_MANAGER, path);
}
