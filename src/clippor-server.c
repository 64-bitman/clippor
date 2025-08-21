#include "clippor-server.h"
#include "clippor-clipboard.h"
#include "clippor-config.h"
#include "clippor-database.h"
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
    GMainLoop *loop; // Optional
};

G_DEFINE_TYPE(ClipporServer, clippor_server, G_TYPE_OBJECT)

static void
clippor_server_dispose(GObject *object)
{
    ClipporServer *self = CLIPPOR_SERVER(object);

    g_clear_object(&self->db);
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

/*
 * Start server using thread default main context and run a main loop until
 * signaled to quit.
 */
gboolean
clippor_server_start(ClipporServer *self, GError **error)
{
    g_assert(CLIPPOR_IS_SERVER(self));
    g_assert(error == NULL || *error == NULL);

    if (!clippor_server_prepare(self, error))
    {
        g_prefix_error_literal(error, "Failed starting server");
        return FALSE;
    }

    self->context = g_main_context_get_thread_default();
    self->loop = g_main_loop_new(self->context, FALSE);

    self->signals[0] = g_unix_signal_add(
        SIGINT, (GSourceFunc)clippor_server_handle_signal, self
    );
    self->signals[1] = g_unix_signal_add(
        SIGTERM, (GSourceFunc)clippor_server_handle_signal, self
    );

    g_debug("Starting server");

    g_main_loop_run(self->loop);
    g_main_loop_unref(self->loop);

    return TRUE;
}
