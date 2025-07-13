#include "wayland-connection.h"
#include "clippor-clipboard.h"
#include "ext-data-control-v1.h"
#include "glib.h"
#include "virtual-keyboard-unstable-v1.h"
#include "wayland-seat.h"
#include "wlr-data-control-unstable-v1.h"
#include <fcntl.h>
#include <glib-object.h>
#include <sys/time.h>
#include <wayland-client.h>

G_DEFINE_QUARK(wayland_connection_error_quark, wayland_connection_error)

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
    void *data;
    WaylandDataDeviceListener *listener;
    WaylandDataProtocol protocol;
};

struct WaylandDataSource
{
    void *proxy;
    void *data;
    WaylandDataSourceListener *listener;
    WaylandDataProtocol protocol;
};

struct WaylandDataOffer
{
    void *proxy;
    void *data;
    GPtrArray *mime_types;
    WaylandDataOfferListener *listener;
    WaylandDataProtocol protocol;
};

typedef struct
{
    GSource source;

    GWeakRef ct;
    GPollFD pfd;
    gboolean is_reading;
} WaylandConnectionSource;

struct _WaylandConnection
{
    GObject parent;

    GSource *source;
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
        GHashTable *seats; // Each key is the name and the value is the
                           // WaylandSeat GObject.

        struct ext_data_control_manager_v1 *ext_data_control_manager_v1;
        struct zwlr_data_control_manager_v1 *zwlr_data_control_manager_v1;
        struct zwp_virtual_keyboard_manager_v1 *zwp_virtual_keyboard_manager_v1;
    } gobjects;

    // In milliseconds
    int data_timeout;
    int connection_timeout;

    gboolean active;
};

G_DEFINE_TYPE(WaylandConnection, wayland_connection, G_TYPE_OBJECT)

typedef enum
{
    PROP_DISPLAY = 1, // An empty string is equivalent to passing NULL
    PROP_DATA_TIMEOUT,
    PROP_CONNECTION_TIMEOUT,
    N_PROPERTIES
} WaylandConnectionProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {NULL};

static gboolean
wayland_connection_setup(WaylandConnection *self, GError **error);
static void wayland_connection_unsetup(WaylandConnection *self);

static void wl_registry_listener_global(
    void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version
);
static void wl_registry_listener_global_remove(
    void *data, struct wl_registry *registry, uint32_t name
);

static struct wl_registry_listener wl_registry_listener = {
    .global = wl_registry_listener_global,
    .global_remove = wl_registry_listener_global_remove,
};

static void
wayland_connection_set_property(
    GObject *object, uint property_id, const GValue *value, GParamSpec *pspec
)
{
    WaylandConnection *self = WAYLAND_CONNECTION(object);
    GError *error = NULL;

    switch ((WaylandConnectionProperty)property_id)
    {
    case PROP_DISPLAY:
    {
        const char *display = g_value_get_string(value);

        if (display == NULL)
            display = g_getenv("WAYLAND_DISPLAY");

        self->display.name = g_strdup(display);
        break;
    }
    case PROP_DATA_TIMEOUT:
        self->data_timeout = g_value_get_int(value);
        break;
    case PROP_CONNECTION_TIMEOUT:
        self->connection_timeout = g_value_get_int(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
    if (error != NULL)
        g_error_free(error);
}

static void
wayland_connection_get_property(
    GObject *object, uint property_id, GValue *value, GParamSpec *pspec
)
{
    WaylandConnection *self = WAYLAND_CONNECTION(object);

    switch ((WaylandConnectionProperty)property_id)
    {
    case PROP_DISPLAY:
        g_value_set_string(value, self->display.name);
        break;
    case PROP_DATA_TIMEOUT:
        g_value_set_int(value, self->data_timeout);
        break;
    case PROP_CONNECTION_TIMEOUT:
        g_value_set_int(value, self->connection_timeout);
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

    wayland_connection_uninstall_source(self);

    g_free(self->display.name);
    g_hash_table_unref(self->gobjects.seats);
    wayland_connection_unsetup(self);

    G_OBJECT_CLASS(wayland_connection_parent_class)->finalize(object);
}

static void wl_log_handler(const char *fmt, va_list args);

static void
wayland_connection_class_init(WaylandConnectionClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->set_property = wayland_connection_set_property;
    gobject_class->get_property = wayland_connection_get_property;

    gobject_class->dispose = wayland_connection_dispose;
    gobject_class->finalize = wayland_connection_finalize;

    obj_properties[PROP_DISPLAY] = g_param_spec_string(
        "display", "Display name", "Name of connected Wayland display", "",
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );
    obj_properties[PROP_DATA_TIMEOUT] = g_param_spec_int(
        "data-timeout", "Data timeout", "Timeout to use when transferring data",
        -1, G_MAXINT, 500, G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );
    obj_properties[PROP_CONNECTION_TIMEOUT] = g_param_spec_int(
        "connection-timeout", "Connection timeout",
        "Timeout to determine if Wayland connection is unresponsive", -1,
        G_MAXINT, 500, G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
    );

    wl_log_set_handler_client(wl_log_handler);
}

static void
wayland_connection_init(WaylandConnection *self)
{
    // Don't free the key since that is owned by the WaylandSeat
    self->gobjects.seats =
        g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_object_unref);
}

/*
 * If display is NULL $WAYLAND_DISPLAY will be used
 */
WaylandConnection *
wayland_connection_new(const char *display_name, GError **error)
{
    g_assert(error == NULL || *error == NULL);

    WaylandConnection *ct =
        g_object_new(WAYLAND_TYPE_CONNECTION, "display", display_name, NULL);

    if (!wayland_connection_setup(ct, error))
    {
        g_assert(error == NULL || error != NULL);

        g_prefix_error(
            error, "Failed connecting to Wayland display '%s': ", display_name
        );

        g_object_unref(ct);

        return NULL;
    }

    return ct;
}

static void
wl_log_handler(const char *fmt, va_list args)
{
    GError *error = g_error_new_valist(
        WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_PROTOCOL, fmt, args
    );

    g_warning("Wayland protocol error: '%s'", error->message);

    g_error_free(error);
}

static gboolean
wayland_connection_setup(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);

    struct wl_display *display = wl_display_connect(self->display.name);

    if (display == NULL)
    {
        g_set_error_literal(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_CONNECT,
            "No such display exists"
        );
        return FALSE;
    }

    struct wl_registry *registry = wl_display_get_registry(display);

    wl_registry_add_listener(registry, &wl_registry_listener, self);

    self->display.proxy = display;
    self->registry.proxy = registry;

    if (!wayland_connection_roundtrip(self, error))
    {
        wayland_connection_unsetup(self);
        return FALSE;
    }

    self->active = TRUE;
    return TRUE;
}

static void
wayland_connection_unsetup(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    if (!self->active)
        return;

    // Remove global objects
    if (self->gobjects.ext_data_control_manager_v1 != NULL)
        ext_data_control_manager_v1_destroy(
            self->gobjects.ext_data_control_manager_v1
        );

    if (self->gobjects.zwlr_data_control_manager_v1 != NULL)
        zwlr_data_control_manager_v1_destroy(
            self->gobjects.zwlr_data_control_manager_v1
        );

    if (self->gobjects.zwp_virtual_keyboard_manager_v1 != NULL)
        zwp_virtual_keyboard_manager_v1_destroy(
            self->gobjects.zwp_virtual_keyboard_manager_v1
        );

    // Disconnect display
    wl_registry_destroy(self->registry.proxy);
    wl_display_disconnect(self->display.proxy);
    self->active = FALSE;
}

/*
 * Sync with wayland_connection_stop()
 */
static void
wl_registry_listener_global(
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
            registry, name, &zwlr_data_control_manager_v1_interface, version
        );
    else if (g_strcmp0(
                 interface, zwp_virtual_keyboard_manager_v1_interface.name
             ) == 0)
        ct->gobjects.zwp_virtual_keyboard_manager_v1 = wl_registry_bind(
            registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1
        );
    else if (g_strcmp0(interface, wl_seat_interface.name) == 0)
    {
        struct wl_seat *seat =
            wl_registry_bind(registry, name, &wl_seat_interface, 2);

        GError *error = NULL;
        WaylandSeat *obj = wayland_seat_new(ct, seat, name, &error);

        if (obj == NULL)
        {
            g_assert(error != NULL);

            g_warning("Failed creating Wayland seat: %s", error->message);
            g_error_free(error);

            // No need to call wl_seat_destroy, finalize function in obj will do
            // that for us.
            return;
        }
        g_hash_table_insert(
            ct->gobjects.seats, wayland_seat_get_name(obj), obj
        );
    }
    else
        return;
}

/*
 * Don't do anything in order to avoid race conditions. Unless it is a seat then
 * unreference it.
 */
static void
wl_registry_listener_global_remove(
    void *data, struct wl_registry *registry G_GNUC_UNUSED, uint32_t name
)
{
    WaylandConnection *ct = data;
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, ct->gobjects.seats);

    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        WaylandSeat *seat = WAYLAND_SEAT(value);

        if (wayland_seat_get_numerical_name(seat) == name)
            g_hash_table_iter_remove(&iter);
    }
}

int
wayland_connection_get_fd(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    return wl_display_get_fd(self->display.proxy);
}

/*
 * Returns NULL if no seats exist. Does not create a new reference of seat. If
 * name is NULL, then the first seat found is used.
 *
 */
WaylandSeat *
wayland_connection_get_seat(WaylandConnection *self, const char *name)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    WaylandSeat *seat = NULL;

    if (name == NULL)
    {
        GHashTableIter iter;

        g_hash_table_iter_init(&iter, self->gobjects.seats);
        g_hash_table_iter_next(&iter, NULL, (gpointer *)&seat);
    }
    else
        seat = g_hash_table_lookup(self->gobjects.seats, name);

    return seat;
}

WaylandSeat *
wayland_connection_match_seat(WaylandConnection *self, GRegex *pattern)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    GHashTableIter iter;
    const char *name;
    WaylandSeat *seat;

    g_hash_table_iter_init(&iter, self->gobjects.seats);

    while (g_hash_table_iter_next(&iter, (gpointer *)&name, (gpointer *)&seat))
        if (g_regex_match(pattern, name, G_REGEX_MATCH_DEFAULT, NULL))
            return seat;

    return NULL;
}

/*
 * Does not create a new reference
 */
GHashTable *
wayland_connection_get_seats(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    return self->gobjects.seats;
}

char *
wayland_connection_get_display_name(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    return self->display.name;
}

struct wl_display *
wayland_connection_get_display(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    return self->display.proxy;
}

gboolean
wayland_connection_flush(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);

    int ret;
    GPollFD pfd = {.fd = wayland_connection_get_fd(self), .events = G_IO_OUT};

    // Send the requests we have made to the compositor, until we have written
    // all the data. Poll in order to check if the display fd is writable, if
    // not, then wait until it is and continue writing or until we timeout.
    while (errno = 0, TRUE)
    {
        ret = wl_display_flush(self->display.proxy);

        if (ret >= 0)
            break;

        if (errno == EAGAIN)
        {
            ret = g_poll(&pfd, 1, self->connection_timeout);

            if (ret == 0)
                g_set_error(
                    error, WAYLAND_CONNECTION_ERROR,
                    WAYLAND_CONNECTION_ERROR_TIMEOUT,
                    "Timeout out while flushing Wayland display '%s'",
                    self->display.name
                );
            else if (ret < 0)
                g_set_error(
                    error, WAYLAND_CONNECTION_ERROR,
                    WAYLAND_CONNECTION_ERROR_FLUSH,
                    "g_poll() failed while flushing Wayland display '%s': %s",
                    self->display.name, g_strerror(errno)
                );
            else
                continue;
        }
        else
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_FLUSH,
                "Failed flushing Wayland display '%s': %s", self->display.name,
                g_strerror(errno)
            );

        return FALSE;
    }
    return TRUE;
}

int
wayland_connection_dispatch(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);
    int num, ret = 0;

    while (wl_display_prepare_read(self->display.proxy) == -1)
        // Dispatch any queued events so that we can start reading
        if (wl_display_dispatch_pending(self->display.proxy) == -1)
        {
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR,
                WAYLAND_CONNECTION_ERROR_DISPATCH,
                "Failed dispatching pending events for display '%s'",
                self->display.name
            );
            return -1;
        }

    // Send any requests before we starting blocking to read display fd
    if (!wayland_connection_flush(self, error))
    {
        g_assert(error == NULL || *error != NULL);
        wl_display_cancel_read(self->display.proxy);
        return -1;
    }
    g_assert(error == NULL || *error == NULL);

    GPollFD fds = {.fd = wayland_connection_get_fd(self), .events = G_IO_IN};

    // Poll until there is data to read from the display fd.
    ret = g_poll(&fds, 1, self->connection_timeout);

    if (ret <= 0)
    {
        if (ret == 0)
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR,
                WAYLAND_CONNECTION_ERROR_TIMEOUT,
                "Timed out polling while dispatching Wayland events for "
                "display "
                "'%s'",
                self->display.name
            );
        else
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR,
                WAYLAND_CONNECTION_ERROR_DISPATCH,
                "g_poll() failed while dispatching Wayland events for display "
                "'%s': %s",
                self->display.name, g_strerror(errno)
            );

        wl_display_cancel_read(self->display.proxy);
        return -1;
    }

    // Read events into the queue
    if (!(fds.revents & G_IO_IN))
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_DISPATCH,
            "Wayland display '%s' closed while dispatching", self->display.name
        );
        return -1;
    }

    ret = wl_display_read_events(self->display.proxy);

    if (ret == -1)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_DISPATCH,
            "Failed reading events for display '%s'", self->display.name
        );
        return -1;
    }

    // Dispatch those events (call the handlers associated for each event)
    if ((num = wl_display_dispatch_pending(self->display.proxy)) == -1)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_DISPATCH,
            "Failed dispatching pending events for display '%s'",
            self->display.name
        );
        return -1;
    }

    return num;
}

static void
wl_callback_listener_done(
    void *data, struct wl_callback *callback, uint32_t cb_data G_GNUC_UNUSED
)
{
    *((gboolean *)data) = TRUE;
    wl_callback_destroy(callback);
}

static struct wl_callback_listener vwl_callback_listener = {
    .done = wl_callback_listener_done
};

gboolean
wayland_connection_roundtrip(WaylandConnection *self, GError **error)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(error == NULL || *error == NULL);

    struct wl_callback *callback;

    int num = 0;
    gboolean done = FALSE;

    // Tell compositor to emit 'done' event after processing all requests we
    // have sent and handling events.
    callback = wl_display_sync(self->display.proxy);

    if (callback == NULL)
    {
        g_set_error(
            error, WAYLAND_CONNECTION_ERROR, WAYLAND_CONNECTION_ERROR_ROUNDTRIP,
            "Failed syncing Wayland display '%s'", self->display.name
        );
        return FALSE;
    }

    wl_callback_add_listener(callback, &vwl_callback_listener, &done);

    int64_t start, now;

    start = g_get_monotonic_time();

    // Wait till we get the done event (which will set `done` to TRUE), with a
    // timeout.
    while (TRUE)
    {
        num = wayland_connection_dispatch(self, error);

        if (num < 0)
            break;
        g_assert(error == NULL || *error == NULL);

        if (done)
            break;

        now = g_get_monotonic_time();

        if (now - start >= self->connection_timeout * 1000)
        {
            num = -1;
            g_set_error(
                error, WAYLAND_CONNECTION_ERROR,
                WAYLAND_CONNECTION_ERROR_TIMEOUT,
                "Timed out while waiting for wl_callback 'done' event for "
                "display '%s'",
                self->display.name
            );
            break;
        }
    }

    if (num < 0)
    {
        g_assert(error == NULL || *error != NULL);
        g_prefix_error_literal(error, "Failed Wayland roundtrip: ");

        if (!done)
            wl_callback_destroy(callback);
        return FALSE;
    }

    return TRUE;
}

static gboolean
wayland_connection_source_prepare(GSource *source, int *timeout_)
{
    WaylandConnectionSource *ws = (WaylandConnectionSource *)source;
    WaylandConnection *ct = g_weak_ref_get(&ws->ct);
    struct wl_display *display = wayland_connection_get_display(ct);
    GError *error = NULL;

    *timeout_ = -1;
    g_object_unref(ct);

    // If -1 is returned then there are still events in the queue to be
    // dispatched.
    if (wl_display_prepare_read(display) == -1)
        return TRUE;

    ws->is_reading = TRUE;

    if (!wayland_connection_flush(ct, &error))
    {
        g_assert(error != NULL);
        g_warning("Failed prepare: %s", error->message);
        g_error_free(error);
    }

    return FALSE;
}

static gboolean
wayland_connection_source_check(GSource *source)
{
    WaylandConnectionSource *ws = (WaylandConnectionSource *)source;
    WaylandConnection *ct = g_weak_ref_get(&ws->ct);
    struct wl_display *display = wayland_connection_get_display(ct);

    ws->is_reading = FALSE;
    g_object_unref(ct);

    if (ws->pfd.revents & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
    {
        wl_display_cancel_read(display);
        g_debug(
            "Wayland display '%s' closed",
            wayland_connection_get_display_name(ct)
        );
        goto fail;
    }
    if (ws->pfd.revents & G_IO_IN)
    {
        if (wl_display_read_events(display) == -1)
        {
            g_warning(
                "Failed reading events on Wayland display '%s'",
                wayland_connection_get_display_name(ct)
            );
            goto fail;
        }
    }
    else
    {
        // In case poll timed out? Honestly sometimes revents is 0 but I'm not
        // sure why since poll shouldn't time out since we set the timeout to
        // -1...
        wl_display_cancel_read(display);
        return FALSE;
    }

    return TRUE;
fail:
    // Make the object inert
    wayland_connection_uninstall_source(ct);
    g_hash_table_remove_all(ct->gobjects.seats);
    wayland_connection_unsetup(ct);

    g_object_unref(ct);
    return FALSE;
}

static gboolean
wayland_connection_source_dispatch(
    GSource *source, GSourceFunc callback, gpointer user_data
)
{
    WaylandConnectionSource *ws = (WaylandConnectionSource *)source;
    WaylandConnection *ct = g_weak_ref_get(&ws->ct);
    struct wl_display *display = wayland_connection_get_display(ct);

    g_object_unref(ct);

    if (wl_display_dispatch_pending(display) == -1)
    {
        g_warning(
            "Failed dispatching events for Wayland display '%s'",
            wayland_connection_get_display_name(ct)
        );
        return G_SOURCE_REMOVE;
    }

    if (callback != NULL)
        callback(user_data);

    return G_SOURCE_CONTINUE;
}

static void
wayland_connection_source_finalize(GSource *source)
{
    WaylandConnectionSource *ws = (WaylandConnectionSource *)source;
    WaylandConnection *ct = g_weak_ref_get(&ws->ct);

    if (ct == NULL)
        return;

    struct wl_display *display = wayland_connection_get_display(ct);

    if (ws->is_reading)
        wl_display_cancel_read(display);

    g_object_unref(ct);
    g_weak_ref_clear(&ws->ct);
}

static GSourceFuncs wayland_connection_source_funcs = {
    .prepare = wayland_connection_source_prepare,
    .check = wayland_connection_source_check,
    .dispatch = wayland_connection_source_dispatch,
    .finalize = wayland_connection_source_finalize
};

void
wayland_connection_install_source(
    WaylandConnection *self, GMainContext *context
)
{
    g_assert(WAYLAND_IS_CONNECTION(self));
    g_assert(self->source == NULL);

    GSource *source = g_source_new(
        &wayland_connection_source_funcs, sizeof(WaylandConnectionSource)
    );
    WaylandConnectionSource *ws = (WaylandConnectionSource *)source;
    char *name = g_strdup_printf(
        "Wayland display '%s'", wayland_connection_get_display_name(self)
    );

    g_source_set_name(source, name);
    g_free(name);

    self->source = source;

    // Don't create a strong ref because then we can't finalize
    // WaylandConnection, but create a weak one since the source finalize
    // callback may come after the WaylandConnection was finalized.
    g_weak_ref_init(&ws->ct, self);

    ws->is_reading = FALSE;

    ws->pfd.fd = wayland_connection_get_fd(self);
    ws->pfd.events = G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL;

    g_source_add_poll(source, &ws->pfd);

    // Set to highest priority so we will always at least be called before other
    // non-Wayland sources that may call Wayland functions.
    g_source_set_priority(source, G_MININT);
    g_source_attach(source, context);
}

void
wayland_connection_uninstall_source(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    if (self->source == NULL)
        return;

    WaylandConnectionSource *ws = (WaylandConnectionSource *)self->source;

    // Make sure we have cancelled any reads, the WaylandConnection may be
    // finalized before the source finalize callback is called.
    if (ws->is_reading)
        wl_display_cancel_read(self->display.proxy);

    g_source_destroy(self->source);
    g_source_unref(self->source);

    self->source = NULL;
}

WaylandDataDeviceManager *
wayland_connection_get_data_device_manager(WaylandConnection *self)
{
    g_assert(WAYLAND_IS_CONNECTION(self));

    WaylandDataDeviceManager *manager = g_new(WaylandDataDeviceManager, 1);

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

void
wayland_data_device_manager_unused(WaylandDataDeviceManager *manager)
{
    if (manager == NULL)
        return;
    // Don't destroy the actual data device manager proxy because we can't just
    // obtain that again without reconnecting.
    g_free(manager);
}

#define WAYLAND_DATA_PROXY_WRAPPER_IS_VALID(type, structure)                   \
    gboolean wayland_##type##_is_valid(structure *type)                        \
    {                                                                          \
        if (type == NULL || type->proxy == NULL)                               \
            return FALSE;                                                      \
        return type->protocol != WAYLAND_DATA_PROTOCOL_NONE;                   \
    }

WAYLAND_DATA_PROXY_WRAPPER_IS_VALID(
    data_device_manager, WaylandDataDeviceManager
)
WAYLAND_DATA_PROXY_WRAPPER_IS_VALID(data_device, WaylandDataDevice)
WAYLAND_DATA_PROXY_WRAPPER_IS_VALID(data_source, WaylandDataSource)
WAYLAND_DATA_PROXY_WRAPPER_IS_VALID(data_offer, WaylandDataOffer)

WaylandDataDevice *
wayland_data_device_manager_get_data_device(
    WaylandDataDeviceManager *manager, WaylandSeat *seat
)
{
    g_assert(wayland_data_device_manager_is_valid(manager));

    WaylandDataDevice *device = g_new0(WaylandDataDevice, 1);

    switch (manager->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        device->proxy = ext_data_control_manager_v1_get_data_device(
            manager->proxy, wayland_seat_get_proxy(seat)
        );
        device->protocol = WAYLAND_DATA_PROTOCOL_EXT;
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        device->proxy = zwlr_data_control_manager_v1_get_data_device(
            manager->proxy, wayland_seat_get_proxy(seat)
        );
        device->protocol = WAYLAND_DATA_PROTOCOL_WLR;
        break;
    default:
        // Shouldn't happen
        g_free(device);
        return NULL;
    }

    return device;
}

WaylandDataSource *
wayland_data_device_manager_create_data_source(
    WaylandDataDeviceManager *manager
)
{
    g_assert(wayland_data_device_manager_is_valid(manager));

    WaylandDataSource *source = g_new0(WaylandDataSource, 1);

    switch (manager->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        source->proxy =
            ext_data_control_manager_v1_create_data_source(manager->proxy);
        source->protocol = WAYLAND_DATA_PROTOCOL_EXT;
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        source->proxy =
            zwlr_data_control_manager_v1_create_data_source(manager->proxy);
        source->protocol = WAYLAND_DATA_PROTOCOL_WLR;
        break;
    default:
        // Shouldn't happen
        g_free(source);
        return NULL;
    }

    return source;
}

static WaylandDataOffer *
wayland_data_offer_create_wrapper(void *proxy, WaylandDataProtocol protocol)
{
    g_assert(proxy != NULL);
    g_assert(protocol != WAYLAND_DATA_PROTOCOL_NONE);

    WaylandDataOffer *offer = g_new0(WaylandDataOffer, 1);

    offer->proxy = proxy;
    offer->protocol = protocol;

    return offer;
}

// Each key is the offer object id and its value is the WaylandDataOffer
// structure. This hash table is used to store/transfer offers in between the
// data_offer, off_clier, and selection events.
static GHashTable *pending_offers = NULL;

#define WAYLAND_DATA_PROXY_DESTROY(type, structure)                            \
    void wayland_data_##type##_destroy(structure *type)                        \
    {                                                                          \
        if (type == NULL)                                                      \
            return;                                                            \
        g_assert(wayland_data_##type##_is_valid(type));                        \
        switch (type->protocol)                                                \
        {                                                                      \
        case WAYLAND_DATA_PROTOCOL_EXT:                                        \
            ext_data_control_##type##_v1_destroy(type->proxy);                 \
            break;                                                             \
        case WAYLAND_DATA_PROTOCOL_WLR:                                        \
            zwlr_data_control_##type##_v1_destroy(type->proxy);                \
            break;                                                             \
        default:                                                               \
            break;                                                             \
        }                                                                      \
        g_free(type);                                                          \
    }

WAYLAND_DATA_PROXY_DESTROY(device, WaylandDataDevice)
WAYLAND_DATA_PROXY_DESTROY(source, WaylandDataSource)

// Data offers have special functions so its manually defined
void
wayland_data_offer_destroy(WaylandDataOffer *offer)
{
    if (offer == NULL)
        return;
    g_assert(wayland_data_offer_is_valid(offer));

    switch (offer->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        ext_data_control_offer_v1_destroy(offer->proxy);
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        zwlr_data_control_offer_v1_destroy(offer->proxy);
        break;
    default:
        break;
    }
    uint32_t id = wl_proxy_get_id(offer->proxy);

    g_hash_table_remove(pending_offers, GUINT_TO_POINTER(id));
    g_ptr_array_unref(offer->mime_types);
    g_free(offer);
}

void
wayland_data_device_set_seletion(
    WaylandDataDevice *device, WaylandDataSource *source,
    ClipporSelectionType selection
)
{
    g_assert(wayland_data_device_is_valid(device));
    g_assert(source == NULL || wayland_data_source_is_valid(source));
    g_assert(selection != CLIPPOR_SELECTION_TYPE_NONE);

    void *proxy = source == NULL ? NULL : source->proxy;

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
    {
        switch (device->protocol)
        {
        case WAYLAND_DATA_PROTOCOL_EXT:
            ext_data_control_device_v1_set_selection(device->proxy, proxy);
            break;
        case WAYLAND_DATA_PROTOCOL_WLR:
            zwlr_data_control_device_v1_set_selection(device->proxy, proxy);
            break;
        default:
            break;
        }
    }
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
    {
        switch (device->protocol)
        {
        case WAYLAND_DATA_PROTOCOL_EXT:
            ext_data_control_device_v1_set_primary_selection(
                device->proxy, proxy
            );
            break;
        case WAYLAND_DATA_PROTOCOL_WLR:
            zwlr_data_control_device_v1_set_primary_selection(
                device->proxy, proxy
            );
            break;
        default:
            break;
        }
    }
}

void
wayland_data_source_offer(WaylandDataSource *source, const char *mime_type)
{
    g_assert(wayland_data_source_is_valid(source));
    g_assert(mime_type != NULL);

    switch (source->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        ext_data_control_source_v1_offer(source->proxy, mime_type);
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        zwlr_data_control_source_v1_offer(source->proxy, mime_type);
        break;
    default:
        break;
    }
}

void
wayland_data_offer_receive(
    WaylandDataOffer *offer, const char *mime_type, int32_t fd
)
{
    g_assert(wayland_data_offer_is_valid(offer));
    g_assert(mime_type != NULL);
    g_assert(fd >= 0);

    switch (offer->protocol)
    {
    case WAYLAND_DATA_PROTOCOL_EXT:
        ext_data_control_offer_v1_receive(offer->proxy, mime_type, fd);
        break;
    case WAYLAND_DATA_PROTOCOL_WLR:
        zwlr_data_control_offer_v1_receive(offer->proxy, mime_type, fd);
        break;
    default:
        break;
    }
}

static void
wayland_data_device_event_data_offer_gen_handler(
    WaylandDataDevice *device, void *offer_proxy
)
{
    WaylandDataOffer *offer =
        wayland_data_offer_create_wrapper(offer_proxy, device->protocol);

    if (device->listener->data_offer == NULL)
    {
        wayland_data_offer_destroy(offer);
        return;
    }

    if (pending_offers == NULL)
        // No destroy function for WaylandDataOffer because that has its own
        // destroy function which will remove itself from the hash table.
        pending_offers =
            g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);

    uint32_t id = wl_proxy_get_id(offer_proxy);

    g_hash_table_insert(pending_offers, GUINT_TO_POINTER(id), offer);

    // 10 mime types is generally the normal maximum from my experience.
    offer->mime_types = g_ptr_array_new_full(10, g_free);

    device->listener->data_offer(device->data, device, offer);
}

static void
wayland_data_device_event_selection_gen_handler(
    WaylandDataDevice *device, void *offer_proxy, ClipporSelectionType selection
)
{
    if (offer_proxy == NULL)
    {
        device->listener->selection(device->data, device, NULL, selection);
        return;
    }

    uint32_t id = wl_proxy_get_id(offer_proxy);
    WaylandDataOffer *offer =
        g_hash_table_lookup(pending_offers, GUINT_TO_POINTER(id));

    if (device->listener->selection == NULL)
    {
        wayland_data_offer_destroy(offer);
        return;
    }

    // Let the callback take ownership of the offer now
    g_hash_table_remove(pending_offers, GUINT_TO_POINTER(id));

    device->listener->selection(device->data, device, offer, selection);
}

#define WAYLAND_DATA_DEVICE_EVENT_DATA_OFFER(device_name, offer_name)          \
    static void device_name##_event_data_offer(                                \
        void *data, struct device_name *device_proxy G_GNUC_UNUSED,            \
        struct offer_name *offer_proxy                                         \
    )                                                                          \
    {                                                                          \
        wayland_data_device_event_data_offer_gen_handler(data, offer_proxy);   \
    }
#define WAYLAND_DATA_DEVICE_EVENT_SELECTION(device_name, offer_name)           \
    static void device_name##_event_selection(                                 \
        void *data, struct device_name *device_proxy G_GNUC_UNUSED,            \
        struct offer_name *offer_proxy                                         \
    )                                                                          \
    {                                                                          \
        wayland_data_device_event_selection_gen_handler(                       \
            data, offer_proxy, CLIPPOR_SELECTION_TYPE_REGULAR                  \
        );                                                                     \
    }                                                                          \
    static void device_name##_event_primary_selection(                         \
        void *data, struct device_name *device_proxy G_GNUC_UNUSED,            \
        struct offer_name *offer_proxy                                         \
    )                                                                          \
    {                                                                          \
        wayland_data_device_event_selection_gen_handler(                       \
            data, offer_proxy, CLIPPOR_SELECTION_TYPE_PRIMARY                  \
        );                                                                     \
    }
#define WAYLAND_DATA_DEVICE_EVENT_FINISHED(device_name)                        \
    static void device_name##_event_finished(                                  \
        void *data, struct device_name *device_proxy G_GNUC_UNUSED             \
    )                                                                          \
    {                                                                          \
        WaylandDataDevice *device = data;                                      \
        if (device->listener->finished != NULL)                                \
            device->listener->finished(device->data, device);                  \
    }

WAYLAND_DATA_DEVICE_EVENT_DATA_OFFER(
    ext_data_control_device_v1, ext_data_control_offer_v1
)
WAYLAND_DATA_DEVICE_EVENT_DATA_OFFER(
    zwlr_data_control_device_v1, zwlr_data_control_offer_v1
)

WAYLAND_DATA_DEVICE_EVENT_SELECTION(
    ext_data_control_device_v1, ext_data_control_offer_v1
)
WAYLAND_DATA_DEVICE_EVENT_SELECTION(
    zwlr_data_control_device_v1, zwlr_data_control_offer_v1
)

WAYLAND_DATA_DEVICE_EVENT_FINISHED(ext_data_control_device_v1)
WAYLAND_DATA_DEVICE_EVENT_FINISHED(zwlr_data_control_device_v1)

static struct ext_data_control_device_v1_listener
    ext_data_control_device_v1_listener = {
        .data_offer = ext_data_control_device_v1_event_data_offer,
        .selection = ext_data_control_device_v1_event_selection,
        .primary_selection = ext_data_control_device_v1_event_primary_selection,
        .finished = ext_data_control_device_v1_event_finished
};
static struct zwlr_data_control_device_v1_listener
    zwlr_data_control_device_v1_listener = {
        .data_offer = zwlr_data_control_device_v1_event_data_offer,
        .selection = zwlr_data_control_device_v1_event_selection,
        .primary_selection =
            zwlr_data_control_device_v1_event_primary_selection,
        .finished = zwlr_data_control_device_v1_event_finished
};

#define WAYLAND_DATA_SOURCE_EVENT_SEND(source_name)                            \
    static void source_name##_event_send(                                      \
        void *data, struct source_name *proxy G_GNUC_UNUSED,                   \
        const char *mime_type, int32_t fd                                      \
    )                                                                          \
    {                                                                          \
        WaylandDataSource *source = data;                                      \
        if (source->listener->send != NULL)                                    \
            source->listener->send(source->data, source, mime_type, fd);       \
    }
#define WAYLAND_DATA_SOURCE_EVENT_CANCELLED(source_name)                       \
    static void source_name##_event_cancelled(                                 \
        void *data, struct source_name *proxy G_GNUC_UNUSED                    \
    )                                                                          \
    {                                                                          \
        WaylandDataSource *source = data;                                      \
        if (source->listener->cancelled != NULL)                               \
            source->listener->cancelled(source->data, source);                 \
    }

WAYLAND_DATA_SOURCE_EVENT_SEND(ext_data_control_source_v1)
WAYLAND_DATA_SOURCE_EVENT_SEND(zwlr_data_control_source_v1)

WAYLAND_DATA_SOURCE_EVENT_CANCELLED(ext_data_control_source_v1)
WAYLAND_DATA_SOURCE_EVENT_CANCELLED(zwlr_data_control_source_v1)

static struct ext_data_control_source_v1_listener
    ext_data_control_source_v1_listener = {
        .send = ext_data_control_source_v1_event_send,
        .cancelled = ext_data_control_source_v1_event_cancelled
};
static struct zwlr_data_control_source_v1_listener
    zwlr_data_control_source_v1_listener = {
        .send = zwlr_data_control_source_v1_event_send,
        .cancelled = zwlr_data_control_source_v1_event_cancelled
};

// Return TRUE to add mime types to array. If offer event handler is not
// specified then it is assumed a return value of TRUE.
#define WAYLAND_DATA_OFFER_EVENT_OFFER(offer_name)                             \
    static void offer_name##_event_offer(                                      \
        void *data, struct offer_name *proxy G_GNUC_UNUSED,                    \
        const char *mime_type                                                  \
    )                                                                          \
    {                                                                          \
        WaylandDataOffer *offer = data;                                        \
        gboolean ret = TRUE;                                                   \
        if (offer->listener->offer != NULL)                                    \
            ret = offer->listener->offer(offer->data, offer, mime_type);       \
        if (!ret)                                                              \
            return;                                                            \
        g_ptr_array_add(offer->mime_types, g_strdup(mime_type));               \
    }

WAYLAND_DATA_OFFER_EVENT_OFFER(ext_data_control_offer_v1)
WAYLAND_DATA_OFFER_EVENT_OFFER(zwlr_data_control_offer_v1)

static struct ext_data_control_offer_v1_listener
    ext_data_control_offer_v1_listener = {
        .offer = ext_data_control_offer_v1_event_offer
};
static struct zwlr_data_control_offer_v1_listener
    zwlr_data_control_offer_v1_listener = {
        .offer = zwlr_data_control_offer_v1_event_offer
};

#define WAYLAND_DATA_PROXY_ADD_LISTENER(type, structure)                       \
    void wayland_data_##type##_add_listener(                                   \
        structure *type, structure##Listener *listener, void *data             \
    )                                                                          \
    {                                                                          \
        g_assert(wayland_data_##type##_is_valid(type));                        \
        g_assert(listener != NULL);                                            \
        g_assert(type->data == NULL);                                          \
        g_assert(type->listener == NULL);                                      \
        type->data = data;                                                     \
        type->listener = listener;                                             \
        switch (type->protocol)                                                \
        {                                                                      \
        case WAYLAND_DATA_PROTOCOL_EXT:                                        \
            ext_data_control_##type##_v1_add_listener(                         \
                type->proxy, &ext_data_control_##type##_v1_listener, type      \
            );                                                                 \
            break;                                                             \
        case WAYLAND_DATA_PROTOCOL_WLR:                                        \
            zwlr_data_control_##type##_v1_add_listener(                        \
                type->proxy, &zwlr_data_control_##type##_v1_listener, type     \
            );                                                                 \
            break;                                                             \
        default:                                                               \
            break;                                                             \
        }                                                                      \
    }

WAYLAND_DATA_PROXY_ADD_LISTENER(device, WaylandDataDevice)
WAYLAND_DATA_PROXY_ADD_LISTENER(source, WaylandDataSource)
WAYLAND_DATA_PROXY_ADD_LISTENER(offer, WaylandDataOffer)

GPtrArray *
wayland_data_offer_get_mime_types(WaylandDataOffer *offer)
{
    g_assert(wayland_data_offer_is_valid(offer));
    return offer->mime_types;
}

/*
 * Make sure to call this before disconnecting all the Wayland connections.
 */
void
wayland_connection_free_static(void)
{
    if (pending_offers != NULL)
    {
        GHashTableIter iter;
        WaylandDataOffer *offer;

        g_hash_table_iter_init(&iter, pending_offers);

        while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&offer))
        {
            wayland_data_offer_destroy(offer);
            g_hash_table_iter_remove(&iter);
        }

        g_hash_table_unref(pending_offers);
        pending_offers = NULL;
    }
}
