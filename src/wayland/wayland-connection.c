#include "wayland-connection.h"
#include "ext-data-control-v1.h"
#include "wayland-seat.h"
#include "wlr-data-control-unstable-v1.h"
#include <wayland-client.h>

G_DEFINE_QUARK(WAYLAND_CONNECTION_ERROR, wayland_connection_error)

// TODO: Use wl_fixes interface to destroy registry

typedef enum
{
    WAYLAND_DATA_PROTOCOL_NONE,
    WAYLAND_DATA_PROTOCOL_EXT,
    WAYLAND_DATA_PROTOCOL_WLR
} WaylandDataProtocol;

struct WaylandDataDeviceManager
{
    void *proxy;
    WaylandDataProtocol protocol;
};

struct WaylandDataDevice
{
    void *proxy;
    WaylandDataProtocol protocol;
    void *data;
    WaylandDataOffer *offer; // Offer for current (data offer)->(selection)
                             // event cycle.
    const WaylandDataDeviceListener *listener;
};

struct WaylandDataSource
{
    void *proxy;
    WaylandDataProtocol protocol;
    void *data;
    const WaylandDataSourceListener *listener;
};

struct WaylandDataOffer
{
    void *proxy;
    WaylandDataProtocol protocol;
    GPtrArray *mime_types;
    void *data;
    const WaylandDataOfferListener *listener;
};

typedef struct
{
    GSource parent;

    WaylandConnection *ct;
    GPollFD pfd;

    gboolean is_reading;
    gboolean error;
} WaylandConnectionSource;

struct _WaylandConnection
{
    GObject parent_instance;
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

    GSource *source;

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

    wayland_connection_uninstall_source(self);

    G_OBJECT_CLASS(wayland_connection_parent_class)->dispose(object);
}

static void
wayland_connection_finalize(GObject *object)
{
    WaylandConnection *self = WAYLAND_CONNECTION(object);

    wayland_connection_stop(self);

    g_hash_table_unref(self->gobjects.seats);
    g_free(self->display.name);

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
    self->gobjects.seats = g_hash_table_new_full(
        g_str_hash, g_str_equal, NULL,
        (GDestroyNotify)wayland_seat_unref_and_inert
    );
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
    WaylandConnection *ct = data;

    if (g_strcmp0(interface, ext_data_control_manager_v1_interface.name) == 0)
        ct->gobjects.ext_data_control_manager_v1 = wl_registry_bind(
            registry, name, &ext_data_control_manager_v1_interface, 1
        );
    else if (g_strcmp0(
                 interface, zwlr_data_control_manager_v1_interface.name
             ) == 0)
        ct->gobjects.zwlr_data_control_manager_v1 = wl_registry_bind(
            registry, name, &zwlr_data_control_manager_v1_interface, 1
        );
    else if (g_strcmp0(interface, wl_seat_interface.name) == 0)
    {
        struct wl_seat *proxy =
            wl_registry_bind(registry, name, &wl_seat_interface, version);

        GError *error = NULL;
        WaylandSeat *seat = wayland_seat_new(ct, proxy, name, &error);

        if (seat == NULL)
        {
            g_warning(
                "Failed creating seat for Wayland display '%s': %s",
                ct->display.name, error->message
            );
            g_error_free(error);
        }
        else
            g_hash_table_insert(
                ct->gobjects.seats, (char *)wayland_seat_get_name(seat), seat
            );
    }
}

/*
 * Don't do anything to avoid race conditions, except if it is a seat, then
 * make it inert and remove it from the table.
 */
static void
wl_seat_listener_event_global_remove(
    void *data, struct wl_registry *registry G_GNUC_UNUSED, uint32_t name
)
{
    WaylandConnection *ct = data;
    GHashTableIter iter;
    WaylandSeat *seat;

    g_hash_table_iter_init(&iter, ct->gobjects.seats);

    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&seat))
    {
        if (wayland_seat_get_numerical_name(seat) == name)
            g_hash_table_iter_remove(&iter);
    }
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

    if (self->active)
        return TRUE;

    self->display.proxy = wl_display_connect(self->display.name);
    g_assert(WAYLAND_IS_CONNECTION(self));

    if (self->display.proxy == NULL)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_CONNECT,
            "Failed connecting to display '%s': Does not exist",
            self->display.name
        );
        return FALSE;
    }

    self->registry.proxy = wl_display_get_registry(self->display.proxy);

    wl_registry_add_listener(self->registry.proxy, &wl_registry_listener, self);

    self->active = TRUE;

    if (!wayland_connection_roundtrip(self, error))
    {
        self->active = FALSE;
        g_prefix_error_literal(error, "Failed starting Wayland connection: ");

        wl_registry_destroy(self->registry.proxy);
        wl_display_disconnect(self->display.proxy);
        return FALSE;
    }

    return TRUE;
}

void
wayland_connection_stop(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    if (!self->active)
        return;

    g_hash_table_remove_all(self->gobjects.seats);

    // Set these to NULL so that we know they are unavailable if we start back
    // up again.
    g_clear_pointer(
        &self->gobjects.ext_data_control_manager_v1,
        ext_data_control_manager_v1_destroy
    );
    g_clear_pointer(
        &self->gobjects.zwlr_data_control_manager_v1,
        zwlr_data_control_manager_v1_destroy
    );

    wl_registry_destroy(self->registry.proxy);
    wl_display_disconnect(self->display.proxy);

    self->active = FALSE;
}

/*
 * Returns -1 if not connected
 */
int
wayland_connection_get_fd(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    return self->active ? wl_display_get_fd(self->display.proxy) : -1;
}

gboolean
wayland_connection_is_active(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    return self->active;
}

/*
 * Flush events in the buffer for connection.
 */
gboolean
wayland_connection_flush(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);

    if (!self->active)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR,
            WAYLAND_CONNECTION_ERROR_NOT_CONNECTED,
            "Failed flushing Wayland display '%s': Not connected",
            self->display.name
        );
        return FALSE;
    }

    GPollFD pfd = {.fd = wayland_connection_get_fd(self), .events = G_IO_OUT};
    int ret = 0;

    // If errno is set to EAGAIN, poll on the fd and flush
    while (errno = 0, (ret = wl_display_flush(self->display.proxy)) == -1 &&
                          errno == EAGAIN)
        if ((ret = g_poll(&pfd, 1, self->connection_timeout)) <= 0)
        {
            if (ret == -1)
                g_set_error(
                    error, WAYLAND_CONNECTION_ERROR,
                    WAYLAND_CONNECTION_ERROR_FLUSH,
                    "Failed flushing Wayland display '%s': Poll failed",
                    self->display.name
                );
            else
                g_set_error(
                    error, WAYLAND_CONNECTION_ERROR,
                    WAYLAND_CONNECTION_ERROR_TIMEOUT,
                    "Failed flushing Wayland display '%s': Timed out",
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

    if (!self->active)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR,
            WAYLAND_CONNECTION_ERROR_NOT_CONNECTED,
            "Failed dispatching Wayland display '%s': Not connected",
            self->display.name
        );
        return FALSE;
    }

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
    int ret;

    if ((ret = g_poll(&pfd, 1, self->connection_timeout)) <= 0)
    {
        if (ret == -1)
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR,
                WAYLAND_CONNECTION_ERROR_DISPATCH,
                "Failed dispatching Wayland display '%s': Poll failed",
                self->display.name
            );
        else
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR,
                WAYLAND_CONNECTION_ERROR_TIMEOUT,
                "Failed dispatching Wayland display '%s': Timed out",
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

    if (!self->active)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR,
            WAYLAND_CONNECTION_ERROR_NOT_CONNECTED,
            "Failed roundtripping Wayland display '%s': Not connected",
            self->display.name
        );
        return FALSE;
    }

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
    int err;

    wl_callback_add_listener(callback, &wl_callback_listener, &done);

    // Dispatch events until we get the done event or if timeout is reached
    while (TRUE)
    {
        if ((err = wayland_connection_dispatch(self, error)) == -1)
        {
            g_prefix_error_literal(
                error, "Failed roundtripping Wayland display: "
            );
            break;
        }
        if (done)
            break;
        if (g_get_monotonic_time() - start >= self->connection_timeout)
        {
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR,
                WAYLAND_CONNECTION_ERROR_TIMEOUT,
                "Failed roundtripping Wayland display '%s': Timed out",
                self->display.name
            );
            err = -1;
            break;
        }
    }

    if (!done)
        wl_callback_destroy(callback);

    return err == -1 ? FALSE : TRUE;
}

static gboolean
source_prepare(GSource *self, int *timeout_)
{
    WaylandConnectionSource *ws = (WaylandConnectionSource *)self;
    struct wl_display *display = ws->ct->display.proxy;
    GError *error = NULL;

    *timeout_ = -1;

    // Prepare to read

    // If -1 is returned, there are still events to be dispatched
    if (wl_display_prepare_read(display) == -1)
        return TRUE;

    if (!wayland_connection_flush(ws->ct, &error))
    {
        g_warning("Failed preparing source: %s", error->message);
        g_error_free(error);

        wl_display_cancel_read(display);
        ws->error = TRUE;
        return TRUE;
    }

    ws->is_reading = TRUE;

    return FALSE;
}

static gboolean
source_check(GSource *self)
{
    WaylandConnectionSource *ws = (WaylandConnectionSource *)self;
    struct wl_display *display = ws->ct->display.proxy;

    if (!ws->is_reading)
        return FALSE;

    if (ws->pfd.revents & (G_IO_HUP | G_IO_ERR))
    {
        g_debug("Wayland connection '%s' closed", ws->ct->display.name);
        goto fail;
    }
    else if (ws->pfd.revents & G_IO_IN)
    {
        if (wl_display_read_events(display) == -1)
        {
            g_warning(
                "Failed reading events on Wayland display '%s'",
                ws->ct->display.name
            );
            goto fail;
        }
        ws->is_reading = FALSE;
        return TRUE;
    }
    else
        // No events happened
        wl_display_cancel_read(display);

    ws->is_reading = FALSE;

    return FALSE;
fail:
    // Stop connection
    wl_display_cancel_read(display);
    ws->is_reading = FALSE;
    ws->error = TRUE;

    return TRUE;
}

static gboolean
source_dispatch(GSource *self, GSourceFunc callback, gpointer user_data)
{
    WaylandConnectionSource *ws = (WaylandConnectionSource *)self;
    struct wl_display *display = ws->ct->display.proxy;

    if (ws->error)
        goto remove;

    if (wl_display_dispatch_pending(display) == -1)
    {
        g_warning(
            "Failed dispatching events on Wayland display '%s'",
            ws->ct->display.name
        );
        goto remove;
    }

    if (callback != NULL)
        callback(user_data);

    return G_SOURCE_CONTINUE;

remove:
    wayland_connection_uninstall_source(ws->ct);
    wayland_connection_stop(ws->ct);
    return G_SOURCE_REMOVE;
}

static void
source_finalize(GSource *self)
{
    WaylandConnectionSource *ws = (WaylandConnectionSource *)self;
    struct wl_display *display = ws->ct->display.proxy;

    if (ws->is_reading)
        wl_display_cancel_read(display);
}

GSourceFuncs g_source_funcs = {
    .prepare = source_prepare,
    .check = source_check,
    .dispatch = source_dispatch,
    .finalize = source_finalize
};

void
wayland_connection_install_source(
    WaylandConnection *self, GMainContext *context
)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    if (!self->active)
        return;

    self->source =
        g_source_new(&g_source_funcs, sizeof(WaylandConnectionSource));

    WaylandConnectionSource *ws = (WaylandConnectionSource *)self->source;

    g_source_set_name(self->source, self->display.name);

    ws->ct = self;
    ws->pfd.fd = wayland_connection_get_fd(self);
    ws->pfd.events = G_IO_IN | G_IO_HUP | G_IO_ERR;

    g_source_add_poll(self->source, &ws->pfd);

    // Make sure sure to always be called first after every poll
    // https://gitlab.gnome.org/GNOME/gtk/-/blob/main/gdk/wayland/gdkeventsource.c
    g_source_set_priority(self->source, G_MININT);
    g_source_attach(self->source, context);
}

void
wayland_connection_uninstall_source(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    if (self->source == NULL)
        return;

    g_source_destroy(self->source);
    g_clear_pointer(&self->source, g_source_unref);
}

#define WAYLAND_DATA_PROXY_IS_VALID(name, struct_name)                         \
    gboolean wayland_data_##name##_is_valid(struct_name *self)                 \
    {                                                                          \
        return self != NULL && self->protocol != WAYLAND_DATA_PROTOCOL_NONE;   \
    }

WAYLAND_DATA_PROXY_IS_VALID(device_manager, WaylandDataDeviceManager)
WAYLAND_DATA_PROXY_IS_VALID(device, WaylandDataDevice)
WAYLAND_DATA_PROXY_IS_VALID(source, WaylandDataSource)
WAYLAND_DATA_PROXY_IS_VALID(offer, WaylandDataOffer)

/*
 * Get a suitable data device manager from connection. Returns NULL if there are
 * none available
 */
WaylandDataDeviceManager *
wayland_connection_get_data_device_manager(WaylandConnection *self)
{
    WaylandDataDeviceManager *manager = g_new(WaylandDataDeviceManager, 1);

    // Prioritize ext-data-control-v1 over wlr-data-control-unstable-v1 because
    // it is newer.
    if (self->gobjects.ext_data_control_manager_v1 != NULL)
    {
        manager->proxy = self->gobjects.ext_data_control_manager_v1;
        manager->protocol = WAYLAND_DATA_PROTOCOL_EXT;
    }
    else if (self->gobjects.zwlr_data_control_manager_v1 != NULL)
    {
        manager->proxy = self->gobjects.zwlr_data_control_manager_v1;
        manager->protocol = WAYLAND_DATA_PROTOCOL_WLR;
    }
    else
    {
        g_free(manager);
        return NULL;
    }

    return manager;
}

WaylandDataDevice *
wayland_data_device_manager_get_data_device(
    WaylandDataDeviceManager *self, WaylandSeat *seat
)
{
    g_assert(wayland_data_device_manager_is_valid(self));
    g_assert(wayland_seat_is_active(seat));

    WaylandDataDevice *device = g_new(WaylandDataDevice, 1);

    switch (self->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        device->proxy = ext_data_control_manager_v1_get_data_device(
            self->proxy, wayland_seat_get_proxy(seat)
        );
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        device->proxy = zwlr_data_control_manager_v1_get_data_device(
            self->proxy, wayland_seat_get_proxy(seat)
        );
        break;
    default:
        // Shouldn't happen
        g_free(device);
        return NULL;
    }
    device->offer = NULL;
    device->protocol = self->protocol;

    return device;
}

WaylandDataSource *
wayland_data_device_manager_create_data_source(WaylandDataDeviceManager *self)
{
    g_assert(wayland_data_device_manager_is_valid(self));

    WaylandDataSource *source = g_new(WaylandDataSource, 1);

    switch (self->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        source->proxy =
            ext_data_control_manager_v1_create_data_source(self->proxy);
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        source->proxy =
            zwlr_data_control_manager_v1_create_data_source(self->proxy);
        break;
    default:
        // Shouldn't happen
        g_free(source);
        return NULL;
    }
    source->protocol = self->protocol;

    return source;
}

WaylandDataOffer *
wayland_data_device_wrap_offer_proxy(WaylandDataDevice *self, void *proxy)
{
    g_assert(wayland_data_device_is_valid(self));

    WaylandDataOffer *offer = g_new(WaylandDataOffer, 1);

    offer->proxy = proxy;
    offer->protocol = self->protocol;
    offer->mime_types = g_ptr_array_new_with_free_func(g_free);

    return offer;
}

#define WAYLAND_DATA_PROXY_DESTROY(name, struct_name)                          \
    void wayland_data_##name##_destroy(struct_name *self)                      \
    {                                                                          \
        if (self == NULL)                                                      \
            return;                                                            \
        g_assert(wayland_data_##name##_is_valid(self));                        \
        switch (self->protocol)                                                \
        {                                                                      \
        case WAYLAND_DATA_PROTOCOL_EXT:                                        \
            ext_data_control_##name##_v1_destroy(self->proxy);                 \
            break;                                                             \
        case WAYLAND_DATA_PROTOCOL_WLR:                                        \
            zwlr_data_control_##name##_v1_destroy(self->proxy);                \
            break;                                                             \
        default:                                                               \
            break;                                                             \
        }                                                                      \
        g_free(self);                                                          \
    }

WAYLAND_DATA_PROXY_DESTROY(device, WaylandDataDevice)
WAYLAND_DATA_PROXY_DESTROY(source, WaylandDataSource)
void
wayland_data_offer_destroy(WaylandDataOffer *self)
{
    if (self == ((void *)0))
        return;

    g_assert(wayland_data_offer_is_valid(self));

    switch (self->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        ext_data_control_offer_v1_destroy(self->proxy);
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        zwlr_data_control_offer_v1_destroy(self->proxy);
        break;
    default:
        break;
    }
    g_ptr_array_unref(self->mime_types);
    g_free(self);
}

void
wayland_data_device_manager_discard(WaylandDataDeviceManager *self)
{
    // Don't want to actually destroy the proxy, we only have that once
    if (self == NULL)
        return;
    g_assert(wayland_data_device_manager_is_valid(self));
    g_free(self);
}

#define DATA_DEVICE_EVENT_DATA_OFFER(device_name, offer_name)                  \
    static void device_name##_listener_event_data_offer(                       \
        void *data, struct device_name *device G_GNUC_UNUSED,                  \
        struct offer_name *offer                                               \
    )                                                                          \
    {                                                                          \
        WaylandDataDevice *self = data;                                        \
        g_assert(self->offer == NULL);                                         \
        self->offer = wayland_data_device_wrap_offer_proxy(self, offer);       \
        self->listener->data_offer(self->data, self, self->offer);             \
    }

#define DATA_DEVICE_EVENT_SELECTION(device_name, offer_name)                   \
    static void device_name##_listener_event_selection(                        \
        void *data, struct device_name *device G_GNUC_UNUSED,                  \
        struct offer_name *offer                                               \
    )                                                                          \
    {                                                                          \
        WaylandDataDevice *self = data;                                        \
        if (offer == NULL)                                                     \
        {                                                                      \
            g_assert(self->offer == NULL);                                     \
            self->listener->selection(                                         \
                self->data, self, NULL, CLIPPOR_SELECTION_TYPE_REGULAR         \
            );                                                                 \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            g_assert(self->offer != NULL);                                     \
            self->listener->selection(                                         \
                self->data, self, self->offer, CLIPPOR_SELECTION_TYPE_REGULAR  \
            );                                                                 \
            self->offer = NULL;                                                \
        }                                                                      \
    }                                                                          \
    static void device_name##_listener_event_primary_selection(                \
        void *data, struct device_name *device G_GNUC_UNUSED,                  \
        struct offer_name *offer                                               \
    )                                                                          \
    {                                                                          \
        WaylandDataDevice *self = data;                                        \
        if (offer == NULL)                                                     \
        {                                                                      \
            g_assert(self->offer == NULL);                                     \
            self->listener->selection(                                         \
                self->data, self, NULL, CLIPPOR_SELECTION_TYPE_PRIMARY         \
            );                                                                 \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            g_assert(self->offer != NULL);                                     \
            self->listener->selection(                                         \
                self->data, self, self->offer, CLIPPOR_SELECTION_TYPE_PRIMARY  \
            );                                                                 \
            self->offer = NULL;                                                \
        }                                                                      \
    }

#define DATA_DEVICE_EVENT_FINISHED(device_name)                                \
    static void device_name##_listener_event_finished(                         \
        void *data, struct device_name *device G_GNUC_UNUSED                   \
    )                                                                          \
    {                                                                          \
        WaylandDataDevice *self = data;                                        \
        self->listener->finished(self->data, self);                            \
    }

DATA_DEVICE_EVENT_DATA_OFFER(
    ext_data_control_device_v1, ext_data_control_offer_v1
)
DATA_DEVICE_EVENT_DATA_OFFER(
    zwlr_data_control_device_v1, zwlr_data_control_offer_v1
)
DATA_DEVICE_EVENT_SELECTION(
    ext_data_control_device_v1, ext_data_control_offer_v1
)
DATA_DEVICE_EVENT_SELECTION(
    zwlr_data_control_device_v1, zwlr_data_control_offer_v1
)
DATA_DEVICE_EVENT_FINISHED(ext_data_control_device_v1)
DATA_DEVICE_EVENT_FINISHED(zwlr_data_control_device_v1)

static struct ext_data_control_device_v1_listener
    ext_data_control_device_v1_listener = {
        .data_offer = ext_data_control_device_v1_listener_event_data_offer,
        .selection = ext_data_control_device_v1_listener_event_selection,
        .primary_selection =
            ext_data_control_device_v1_listener_event_primary_selection,
        .finished = ext_data_control_device_v1_listener_event_finished
};
static const struct zwlr_data_control_device_v1_listener
    zwlr_data_control_device_v1_listener = {
        .data_offer = zwlr_data_control_device_v1_listener_event_data_offer,
        .selection = zwlr_data_control_device_v1_listener_event_selection,
        .primary_selection =
            zwlr_data_control_device_v1_listener_event_primary_selection,
        .finished = zwlr_data_control_device_v1_listener_event_finished
};

#define DATA_SOURCE_EVENT_SEND(source_name)                                    \
    static void source_name##_listener_event_send(                             \
        void *data, struct source_name *source G_GNUC_UNUSED,                  \
        const char *mime_type, int fd                                          \
    )                                                                          \
    {                                                                          \
        WaylandDataSource *self = data;                                        \
        self->listener->send(self->data, self, mime_type, fd);                 \
    }

#define DATA_SOURCE_EVENT_CANCELLED(source_name)                               \
    static void source_name##_Listener_event_cancelled(                        \
        void *data, struct source_name *source G_GNUC_UNUSED                   \
    )                                                                          \
    {                                                                          \
        WaylandDataSource *self = data;                                        \
        self->listener->cancelled(self->data, self);                           \
    }

DATA_SOURCE_EVENT_SEND(ext_data_control_source_v1)
DATA_SOURCE_EVENT_SEND(zwlr_data_control_source_v1)
DATA_SOURCE_EVENT_CANCELLED(ext_data_control_source_v1)
DATA_SOURCE_EVENT_CANCELLED(zwlr_data_control_source_v1)

static const struct ext_data_control_source_v1_listener
    ext_data_control_source_v1_listener = {
        .send = ext_data_control_source_v1_listener_event_send,
        .cancelled = ext_data_control_source_v1_Listener_event_cancelled
};
static const struct zwlr_data_control_source_v1_listener
    zwlr_data_control_source_v1_listener = {
        .send = zwlr_data_control_source_v1_listener_event_send,
        .cancelled = zwlr_data_control_source_v1_Listener_event_cancelled
};

#define DATA_OFFER_EVENT_OFFER(offer_name)                                     \
    static void offer_name##_listener_event_offer(                             \
        void *data, struct offer_name *offer G_GNUC_UNUSED,                    \
        const char *mime_type                                                  \
    )                                                                          \
    {                                                                          \
        WaylandDataOffer *self = data;                                         \
        if (self->listener->offer(self->data, self, mime_type))                \
            g_ptr_array_add(self->mime_types, g_strdup(mime_type));            \
    }

DATA_OFFER_EVENT_OFFER(ext_data_control_offer_v1)
DATA_OFFER_EVENT_OFFER(zwlr_data_control_offer_v1)

static const struct ext_data_control_offer_v1_listener
    ext_data_control_offer_v1_listener = {
        .offer = ext_data_control_offer_v1_listener_event_offer
};
static const struct zwlr_data_control_offer_v1_listener
    zwlr_data_control_offer_v1_listener = {
        .offer = zwlr_data_control_offer_v1_listener_event_offer
};

#define WAYLAND_DATA_PROXY_ADD_LISTENER(name, struct_name)                     \
    void wayland_data_##name##_add_listener(                                   \
        struct_name *self, const struct_name##Listener *listener, void *data   \
    )                                                                          \
    {                                                                          \
        g_assert(wayland_data_##name##_is_valid(self));                        \
        self->data = data;                                                     \
        self->listener = listener;                                             \
        switch (self->protocol)                                                \
        {                                                                      \
        case WAYLAND_DATA_PROTOCOL_EXT:                                        \
            ext_data_control_##name##_v1_add_listener(                         \
                self->proxy, &ext_data_control_##name##_v1_listener, self      \
            );                                                                 \
            break;                                                             \
        case WAYLAND_DATA_PROTOCOL_WLR:                                        \
            zwlr_data_control_##name##_v1_add_listener(                        \
                self->proxy, &zwlr_data_control_##name##_v1_listener, self     \
            );                                                                 \
            break;                                                             \
        default:                                                               \
            break;                                                             \
        }                                                                      \
    }

WAYLAND_DATA_PROXY_ADD_LISTENER(device, WaylandDataDevice)
WAYLAND_DATA_PROXY_ADD_LISTENER(source, WaylandDataSource)
WAYLAND_DATA_PROXY_ADD_LISTENER(offer, WaylandDataOffer)

void
wayland_data_device_set_selection(
    WaylandDataDevice *self, WaylandDataSource *source,
    ClipporSelectionType selection
)
{
    g_assert(wayland_data_device_is_valid(self));
    g_assert(wayland_data_source_is_valid(source));
    g_assert(self->protocol == source->protocol);
    g_assert(selection != CLIPPOR_SELECTION_TYPE_NONE);

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
    {
        switch (self->protocol)
        {
        case WAYLAND_DATA_PROTOCOL_EXT:
            ext_data_control_device_v1_set_selection(
                self->proxy, source->proxy
            );
            break;
        case WAYLAND_DATA_PROTOCOL_WLR:
            zwlr_data_control_device_v1_set_selection(
                self->proxy, source->proxy
            );
            break;
        default:
            break;
        }
    }
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
    {
        switch (self->protocol)
        {
        case WAYLAND_DATA_PROTOCOL_EXT:
            ext_data_control_device_v1_set_primary_selection(
                self->proxy, source->proxy
            );
            break;
        case WAYLAND_DATA_PROTOCOL_WLR:
            zwlr_data_control_device_v1_set_primary_selection(
                self->proxy, source->proxy
            );
            break;
        default:
            break;
        }
    }
}

void
wayland_data_source_offer(WaylandDataSource *self, const char *mime_type)
{
    g_assert(wayland_data_source_is_valid(self));
    g_assert(mime_type != NULL);

    switch (self->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        ext_data_control_source_v1_offer(self->proxy, mime_type);
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        zwlr_data_control_source_v1_offer(self->proxy, mime_type);
        break;
    default:
        break;
    }
}

void
wayland_data_offer_receive(
    WaylandDataOffer *self, const char *mime_type, int fd
)
{
    g_assert(wayland_data_offer_is_valid(self));
    g_assert(mime_type != NULL);
    g_assert(fd >= 0);

    switch (self->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        ext_data_control_offer_v1_receive(self->proxy, mime_type, fd);
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        zwlr_data_control_offer_v1_receive(self->proxy, mime_type, fd);
        break;
    default:
        break;
    }
}

const GPtrArray *
wayland_data_offer_get_mime_types(WaylandDataOffer *self)
{
    g_assert(wayland_data_offer_is_valid(self));

    return self->mime_types;
}
