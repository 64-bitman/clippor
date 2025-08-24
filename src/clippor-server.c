#include "clippor-server.h"
#include "clippor-clipboard.h"
#include "clippor-config.h"
#include "clippor-database.h"
#include "com.github.Clippor.h"
#include "modules.h"
#include "wayland-connection.h"
#include <glib-object.h>
#include <glib-unix.h>
#include <glib.h>
#include <gmodule.h>

G_DEFINE_QUARK(SERVER_ERROR, server_error)

struct _ClipporServer
{
    GObject parent_instance;

    ClipporConfig *cfg;
    ClipporDatabase *db;

    uint signals[2]; // Source ids for each signal
    GMainContext *context;
    GMainLoop *loop;

    // DBus service for server
    struct
    {
        gboolean available;
        gboolean got_name;
        gboolean got_bus;

        uint name_id;
        GDBusConnection *connection;

        GDBusObjectManagerServer *clipboards_manager;
    } dbus;
};

G_DEFINE_TYPE(ClipporServer, clippor_server, G_TYPE_OBJECT)

static void
clippor_server_dispose(GObject *object)
{
    ClipporServer *self = CLIPPOR_SERVER(object);

    g_clear_object(&self->db);
    g_clear_pointer(&self->context, g_main_context_unref);
    g_clear_pointer(&self->cfg, clippor_config_unref);

    G_OBJECT_CLASS(clippor_server_parent_class)->dispose(object);
}

static void
clippor_server_finalize(GObject *object)
{
    G_OBJECT_CLASS(clippor_server_parent_class)->finalize(object);
}

static void
clippor_server_class_init(ClipporServerClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->dispose = clippor_server_dispose;
    gobject_class->finalize = clippor_server_finalize;

    g_dbus_error_register_error(
        SERVER_ERROR, SERVER_ERROR_OBJECT_EXISTS,
        "com.github.Clippor.Error.ObjectExists"
    );
    g_dbus_error_register_error(
        SERVER_ERROR, SERVER_ERROR_OBJECT_CREATE,
        "com.github.Clippor.Error.ObjectCreate"
    );
}

static void
clippor_server_init(ClipporServer *self G_GNUC_UNUSED)
{
}

/*
 * If NULL is passed for "db", then all history is done in memory.
 */
ClipporServer *
clippor_server_new(ClipporConfig *cfg, ClipporDatabase *db)
{
    g_assert(cfg != NULL);
    g_assert(db == NULL || CLIPPOR_IS_DATABASE(db));

    ClipporServer *server = g_object_new(CLIPPOR_TYPE_SERVER, NULL);

    server->db = db == NULL ? NULL : g_object_ref(db);
    server->cfg = clippor_config_ref(cfg);

    return server;
}

static gboolean
clippor_server_prepare(ClipporServer *self, GError **error)
{
    if (self->db != NULL)
    {
        // Prepare clipboards
        for (uint i = 0; i < self->cfg->clipboards->len; i++)
        {
            ClipporClipboard *cb = self->cfg->clipboards->pdata[i];

            if (!clippor_clipboard_set_database(cb, self->db, error))
                return FALSE;
        }
    }
    if (WAYLAND_FUNCS.available)
    {
        // Bind wayland connections to main context
        for (uint i = 0; i < self->cfg->wayland_connections->len; i++)
        {
            WaylandConnection *ct = self->cfg->wayland_connections->pdata[i];

            WAYLAND_FUNCS.connection_install_source(ct, self->context);
        }
    }

    return TRUE;
}

static gboolean
clippor_server_handle_signal(ClipporServer *self)
{
    g_assert(CLIPPOR_IS_SERVER(self));

    g_message("Exiting...");

    for (uint i = 0; i < G_N_ELEMENTS(self->signals); i++)
        g_source_remove(self->signals[i]);

    g_main_loop_quit(self->loop);

    return G_SOURCE_REMOVE;
}

static void clippor_server_start_dbus(ClipporServer *self);
static void clippor_server_stop_dbus(ClipporServer *self);

/*
 * Start server using thread default main context and run a main loop until
 * signaled to quit.
 */
gboolean
clippor_server_start(ClipporServer *self, GError **error)
{
    g_assert(CLIPPOR_IS_SERVER(self));
    g_assert(error == NULL || *error == NULL);

    g_debug("Starting server");

    self->context = g_main_context_ref_thread_default();
    self->loop = g_main_loop_new(self->context, FALSE);

    clippor_server_start_dbus(self);

    if (!clippor_server_prepare(self, error))
    {
        g_prefix_error_literal(error, "Failed starting server");
        clippor_server_stop_dbus(self);
        g_main_context_unref(self->context);
        g_main_loop_unref(self->loop);
        return FALSE;
    }

    self->signals[0] = g_unix_signal_add(
        SIGINT, (GSourceFunc)clippor_server_handle_signal, self
    );
    self->signals[1] = g_unix_signal_add(
        SIGTERM, (GSourceFunc)clippor_server_handle_signal, self
    );

    g_main_loop_run(self->loop);
    g_main_loop_unref(self->loop);
    clippor_server_stop_dbus(self);

    return TRUE;
}

static void
on_bus_acquired(
    GDBusConnection *connection, const gchar *name G_GNUC_UNUSED,
    gpointer user_data
)
{
    ClipporServer *server = user_data;

    g_debug("DBus service: acquired the session bus connectionn");
    server->dbus.got_bus = TRUE;

    server->dbus.connection = g_object_ref(connection);
}

static void
on_name_acquired(
    GDBusConnection *connection G_GNUC_UNUSED, const gchar *name,
    gpointer user_data
)
{
    ClipporServer *server = user_data;

    g_debug("DBus service: acquired the name '%s'", name);
    server->dbus.got_name = TRUE;

    if (server->dbus.got_bus)
        g_main_loop_quit(server->loop);
}

static void
on_name_lost(
    GDBusConnection *connection G_GNUC_UNUSED, const gchar *name,
    gpointer user_data
)
{
    ClipporServer *server = user_data;

    g_debug("DBus service: lost the name '%s'", name);

    server->dbus.available = FALSE;

    if (server->dbus.got_bus || connection == NULL)
        g_main_loop_quit(server->loop);
}

/*
 * Own the DBus name and start the service. Returns FALSE on error.
 */
static void
clippor_server_start_dbus(ClipporServer *self)
{
    g_assert(CLIPPOR_IS_SERVER(self));

    self->dbus.name_id = g_bus_own_name(
        G_BUS_TYPE_SESSION, "com.github.Clippor",
        G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE, on_bus_acquired, on_name_acquired,
        on_name_lost, self, NULL
    );

    self->dbus.available = TRUE; // Will be set to FALSE if needed

    // Run main loop until we acquire bus (or not)
    g_main_loop_run(self->loop);

    if (!self->dbus.available && !self->dbus.got_bus && !self->dbus.got_name)
        // Connection to bus cannot be made
        g_debug("DBus service: failed creating connection to session bus");
    else if (!self->dbus.available && self->dbus.got_bus)
        // Name cannot be obtained
        g_debug("DBus service: cannot obtain name 'com.github.Clippor'");
    else
    {
        g_debug("Dbus service: success");

        DBusClippor *object;
        g_autoptr(GError) error = NULL;

        object = dbus_clippor_skeleton_new();

        if (!g_dbus_interface_skeleton_export(
                G_DBUS_INTERFACE_SKELETON(object), self->dbus.connection,
                "/com/github/Clippor", &error
            ))
            g_warning(
                "Failed creating DBus object at '/com/github/Clippor': %s",
                error->message
            );
        else
        {
            self->dbus.clipboards_manager = g_dbus_object_manager_server_new(
                "/com/github/Clippor/Clipboards"
            );

            g_dbus_object_manager_server_set_connection(
                self->dbus.clipboards_manager, self->dbus.connection
            );
        }

        return;
    }
    self->dbus.available = FALSE;
}

static void
clippor_server_stop_dbus(ClipporServer *self)
{
    g_assert(CLIPPOR_IS_SERVER(self));

    if (self->dbus.clipboards_manager != NULL)
        g_object_unref(self->dbus.clipboards_manager);

    g_bus_unown_name(self->dbus.name_id);
    if (self->dbus.connection != NULL)
        g_object_unref(self->dbus.connection);
}
