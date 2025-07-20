#include "wayland-connection.h"
#include "ext-data-control-v1.h"
#include "wlr-data-control-unstable-v1.h"
#include <wayland-client.h>

G_DEFINE_QUARK(WAYLAND_CONNECTION_ERROR, wayland_connection_error)

struct _WaylandConnection
{
    struct
    {
        struct wl_display *proxy;
        char *name;
    } display;

    struct
    {
        struct wl_registry *proxy;
    } registry;

    struct
    {
        GHashTable *seats;

        struct ext_data_control_manager_v1 *ext_data_control_manager_v1;
        struct zwlr_data_control_manager_v1 *zwlr_data_control_manager_v1;
    } gobjects;

    int connection_timeout;
    int data_timeout;

    gboolean active;
};

G_DEFINE_TYPE(WaylandConnection, wayland_connection, G_TYPE_OBJECT)

typedef enum
{
    PROP_CONNECTION_TIMEOUT = 1,
    PROP_DATA_TIMEOUT,
    N_PROPERTIES
} WaylandConnectionProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {NULL};

static void
wayland_connection_set_property(
    GObject *object, guint property_id, const GValue *value, GParamSpec *pspec
)
{
    WaylandConnection *self = WAYLAND_CONNECTION(object);

    switch (property_id)
    {
    case PROP_CONNECTION_TIMEOUT:
        self->connection_timeout = g_value_get_int(value);
        break;
    case PROP_DATA_TIMEOUT:
        self->data_timeout = g_value_get_int(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
wayland_connection_get_property(
    GObject *object, guint property_id, GValue *value, GParamSpec *pspec
)
{
    WaylandConnection *self = WAYLAND_CONNECTION(object);

    switch (property_id)
    {
    case PROP_CONNECTION_TIMEOUT:
        g_value_set_int(value, self->connection_timeout);
        break;
    case PROP_DATA_TIMEOUT:
        g_value_set_int(value, self->data_timeout);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
wayland_connection_dispose(GObject *object)
{
    WaylandConnection *self = WAYLAND_CONNECTION(object);

    g_hash_table_remove_all(self->gobjects.seats);

    G_OBJECT_CLASS(wayland_connection_parent_class)->dispose(object);
}

static void
wayland_connection_finalize(GObject *object)
{
    WaylandConnection *self = WAYLAND_CONNECTION(object);

    g_hash_table_unref(self->gobjects.seats);

    G_OBJECT_CLASS(wayland_connection_parent_class)->finalize(object);
}

static void
wayland_connection_class_init(WaylandConnectionClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->set_property = wayland_connection_set_property;
    gobject_class->get_property = wayland_connection_get_property;

    gobject_class->dispose = wayland_connection_dispose;
    gobject_class->finalize = wayland_connection_finalize;

    obj_properties[PROP_CONNECTION_TIMEOUT] = g_param_spec_int(
        "connection-timeout", "Connection timeout",
        "Timeout to use when polling display", -1, G_MAXINT, 500,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );
    obj_properties[PROP_DATA_TIMEOUT] = g_param_spec_int(
        "data-timeout", "Data timeout", "Timeout to use when transferring data",
        -1, G_MAXINT, 500, G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
    );
}

static void
wayland_connection_init(WaylandConnection *self)
{
    self->gobjects.seats =
        g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_object_unref);
}

/*
 * If "display" is NULL, then $WAYLAND_DISPLAY is used.
 */
WaylandConnection *
wayland_connection_new(const char *display)
{
    WaylandConnection *ct = g_object_new(WAYLAND_TYPE_CONNECTION, NULL);

    ct->display.name = g_strdup(display);

    return ct;
}

static void
wl_seat_listener_event_global(
    void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version
)
{
}

static void
wl_seat_listener_event_global_remove(
    void *data, struct wl_registry *registry, uint32_t name
)
{
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = wl_seat_listener_event_global,
    .global_remove = wl_seat_listener_event_global_remove
};

gboolean
wayland_connection_start(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);

    self->display.proxy = wl_display_connect(self->display.name);

    if (self->display.proxy == NULL)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_CONNECT,
            "Failed connecting to display '%s': Does not exist",
            self->display.name
        );
        return FALSE;
    }

    self->active = TRUE;

    return TRUE;
}

void
wayland_connection_stop(WaylandConnection *self)
{

    self->active = FALSE;
    g_assert(WAYLAND_IS_CONNECTION(self));
}

int
wayland_connection_get_fd(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    return wl_display_get_fd(self->display.proxy);
}

/*
 * Flush events in the buffer for connection.
 */
gboolean
wayland_connection_flush(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);

    GPollFD pfd = {.fd = wayland_connection_get_fd(self), .events = G_IO_OUT};
    int ret = 0;

    // If errno is set to EAGAIN, poll on the fd and flush
    while (errno = 0, (ret = wl_display_flush(self->display.proxy)) == -1 &&
                          errno == EAGAIN)
        if (g_poll(&pfd, 1, self->connection_timeout) == -1)
        {
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_FLUSH,
                "Failed flushing Wayland display '%s': Poll failed",
                self->display.name
            );
            return FALSE;
        }

    return TRUE;
}

/*
 * Dispatch events for display. Returns number of events dispatched else -1 on
 * error.
 */
int
wayland_connection_dispatch(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);

    // Dispatch any pending events still in the queue
    while (wl_display_prepare_read(self->display.proxy) == -1)
        if (wl_display_dispatch_pending(self->display.proxy) == -1)
        {
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR,
                WAYLAND_CONNECTION_ERROR_DISPATCH,
                "Failed dispatching Wayland display '%s': Failed dispatching "
                "pending events",
                self->display.name
            );
            return -1;
        }

    if (!wayland_connection_flush(self, error))
    {
        g_prefix_error_literal(error, "Failed dispatching Wayland display: ");
        return -1;
    }

    GPollFD pfd = {.fd = wayland_connection_get_fd(self), .events = G_IO_IN};

    if (g_poll(&pfd, 1, self->connection_timeout) == -1)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_DISPATCH,
            "Failed dispatching Wayland display '%s': Poll failed",
            self->display.name
        );
        wl_display_cancel_read(self->display.proxy);
        return -1;
    }

    if (wl_display_read_events(self->display.proxy) == -1)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_DISPATCH,
            "Failed dispatching Wayland display '%s': Failed reading events",
            self->display.name
        );
        wl_display_cancel_read(self->display.proxy); // Not sure if this is
                                                     // needed...
        return -1;
    }

    int num;

    if ((num = wl_display_dispatch_pending(self->display.proxy)) == -1)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_DISPATCH,
            "Failed dispatching Wayland display '%s': Failed dispatching "
            "pending events",
            self->display.name
        );
        return -1;
    }

    return num;
}

static void
wl_callback_listener_event_done(
    void *data, struct wl_callback *callback, uint32_t serial G_GNUC_UNUSED
)
{
    *(gboolean *)data = TRUE;
    wl_callback_destroy(callback);
}

struct wl_callback_listener wl_callback_listener = {
    .done = wl_callback_listener_event_done
};

/*
 * Do a roundtrip for connection.
 */
gboolean
wayland_connection_roundtrip(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);

    struct wl_callback *callback = wl_display_sync(self->display.proxy);

    if (callback == NULL)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_ROUNDTRIP,
            "Failed roundtripping Wayland display '%s': Failed creating "
            "callback",
            self->display.name
        );
        return FALSE;
    }

    gboolean done = FALSE;
    int64_t start = g_get_monotonic_time();

    wl_callback_add_listener(callback, &wl_callback_listener, &done);

    // Dispatch events until we get the done event or if timeout is reached
    while (!done)
    {
        if (wayland_connection_dispatch(self, error) == -1)
        {
            g_prefix_error_literal(
                error, "Failed roundtripping Wayland display: "
            );
            goto fail;
        }
        if (g_get_monotonic_time() - start >= self->connection_timeout)
            break;
    }

    if (!done)
    {
fail:
        wl_callback_destroy(callback);
        return FALSE;
    }

    return TRUE;
}
