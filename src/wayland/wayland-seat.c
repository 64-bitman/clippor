#include "wayland-seat.h"
#include "clippor-clipboard.h"
#include "wayland-connection.h"
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <glib-object.h>
#include <wayland-client.h>

G_DEFINE_QUARK(wayland_seat_error_quark, wayland_seat_error)

// When we receive a new selection from a seat, we notify the clipboard we are
// under, but don't immediately become the owner of the selection for the seat.
// Instead, wait until we detect there is no source client for the selection,
// then we will become the owner. This plays nicely with primary selection since
// clients would constantly have to be regaining ownership of the selection
// everytime the text selection changes + we also preserve their mime types,
// since we can't handle all mime types.

struct _WaylandSeat
{
    GObject parent;

    // Clipboard this client is attached to, NULL if it is attached to none.
    ClipporClipboard *regular_cb;
    ClipporClipboard *primary_cb;

    struct wl_seat *proxy;

    gchar *name;
    guint32 numerical_name;
    enum wl_seat_capability capabilities;

    gboolean active; // If this seat is actively being used to get/set clipboard
                     // data.

    WaylandConnection *ct; // Parent connection

    GPtrArray *mime_types; // Temporary array for holding mime types until we
                           // get information in which selection they belong to.

    WaylandDataDeviceManager *data_device_manager;
    WaylandDataDevice *data_device;
    WaylandDataSource *data_source;
};

G_DEFINE_TYPE(WaylandSeat, wayland_seat, G_TYPE_OBJECT)

typedef enum
{
    PROP_NAME = 1,
    PROP_ACTIVE,
    PROP_CONNECTION,
    PROP_CLIPBOARD_REGULAR, // Notify signal will only be send when actually
                            // changed.
    PROP_CLIPBOARD_PRIMARY, // Same as above ^.
    N_PROPERTIES
} WaylandSeatProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {
    NULL,
};

typedef enum
{
    SIGNAL_SELECTION, // details: ::regular , ::primary
    N_SIGNALS
} WaylandSeatSignal;

static guint obj_signals[N_SIGNALS] = {0};

static void selection_default_cb(WaylandSeat *self, GHashTable *mime_types);

static void
on_removed_from_clipboard(GObject *object, GObject *client, gpointer data);

static void on_ct_dispose(gpointer data, GObject *object);

static gboolean wayland_seat_start(WaylandSeat *self, GError **error);
static void wayland_seat_stop(WaylandSeat *self);

static void wayland_seat_update_selections(WaylandSeat *self, gboolean force);

static void
wayland_seat_set_property(
    GObject *object, guint property_id, const GValue *value, GParamSpec *pspec
)
{
    WaylandSeat *self = WAYLAND_SEAT(object);
    ClipporClipboard **cb;
    GError *error = NULL;

    switch ((WaylandSeatProperty)property_id)
    {
    case PROP_ACTIVE:
    {
        gboolean new = g_value_get_boolean(value);

        if (new == self->active)
            break;

        if (!wayland_connection_is_active(self->ct))
        {
            g_info(
                "Cannot start Wayland seat '%s', parent connection is not "
                "active for display '%s'",
                self->name, wayland_connection_get_display_name(self->ct)
            );
            break;
        }

        if (new)
        {
            if (!wayland_seat_start(self, &error))
            {
                g_assert(error != NULL);
                g_info(
                    "Failed starting Wayland seat '%s': %s", self->name,
                    error->message
                );
            }
        }
        else
        {
            wayland_seat_stop(self);
            g_assert(error == NULL);
        }

        break;
    }
    case PROP_CONNECTION:
        // Don't create a new reference to ct because it will outlive us anyways
        self->ct = g_value_get_object(value);
        g_object_weak_ref(G_OBJECT(self->ct), on_ct_dispose, self);
        break;
    case PROP_CLIPBOARD_PRIMARY:
        cb = &self->primary_cb;
        goto cb_changed;
    case PROP_CLIPBOARD_REGULAR:
        cb = &self->regular_cb;
cb_changed:
        if (*cb == CLIPPOR_CLIPBOARD(g_value_get_object(value)))
            // Don't send notify signal if nothing changed
            break;

        if (*cb != NULL)
            g_object_unref(*cb);
        *cb = g_value_dup_object(value);

        if (*cb == NULL)
            return;

        // Update seat so it uses the selection for the most recent entry in the
        // new clipboard.
        wayland_seat_update_selections(self, TRUE);

        // Get notified when clipboard may have possibly removed client from its
        // list
        g_signal_connect(
            *cb, "client-removed", G_CALLBACK(on_removed_from_clipboard), self
        );

        g_object_notify_by_pspec(object, pspec);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
    if (error != NULL)
        g_error_free(error);
}

static void
wayland_seat_get_property(
    GObject *object, guint property_id, GValue *value, GParamSpec *pspec
)
{
    WaylandSeat *self = WAYLAND_SEAT(object);

    switch ((WaylandSeatProperty)property_id)
    {
    case PROP_NAME:
        g_value_set_string(value, self->name);
        break;
    case PROP_ACTIVE:
        g_value_set_boolean(value, self->active);
        break;
    case PROP_CONNECTION:
        g_value_set_object(value, self->ct);
        break;
    case PROP_CLIPBOARD_REGULAR:
        g_value_set_object(value, self->regular_cb);
        break;
    case PROP_CLIPBOARD_PRIMARY:
        g_value_set_object(value, self->primary_cb);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
wayland_seat_dispose(GObject *object)
{
    WaylandSeat *self = WAYLAND_SEAT(object);

    g_clear_object(&self->regular_cb);
    g_clear_object(&self->primary_cb);

    G_OBJECT_CLASS(wayland_seat_parent_class)->dispose(object);
}

static void
wayland_seat_finalize(GObject *object)
{
    WaylandSeat *self = WAYLAND_SEAT(object);

    g_free(self->name);

    if (self->proxy != NULL)
        wl_seat_destroy(self->proxy);

    if (self->mime_types != NULL)
        g_ptr_array_unref(self->mime_types);

    if (self->active)
        wayland_seat_stop(self);

    G_OBJECT_CLASS(wayland_seat_parent_class)->finalize(object);
}

static void
wayland_seat_class_init(WaylandSeatClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->set_property = wayland_seat_set_property;
    gobject_class->get_property = wayland_seat_get_property;

    gobject_class->dispose = wayland_seat_dispose;
    gobject_class->finalize = wayland_seat_finalize;

    obj_properties[PROP_NAME] = g_param_spec_string(
        "name", "Seat name", "Name of Wayland seat", "", G_PARAM_READABLE
    );
    obj_properties[PROP_ACTIVE] = g_param_spec_boolean(
        "active", "Active", "If Wayland seat selection is managed by Clippor",
        FALSE, G_PARAM_READWRITE
    );
    obj_properties[PROP_CONNECTION] = g_param_spec_object(
        "connection", "Connection", "Parent Wayland connection",
        WAYLAND_TYPE_CONNECTION, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    );
    obj_properties[PROP_CLIPBOARD_REGULAR] = g_param_spec_object(
        "clipboard-regular", "Regular clipboard",
        "Clipboard this client is attached to for the regular selection",
        CLIPPOR_TYPE_CLIPBOARD, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY
    );
    obj_properties[PROP_CLIPBOARD_PRIMARY] = g_param_spec_object(
        "clipboard-primary", "Primary clipboard",
        "Clipboard this client is attached to for the primary selection",
        CLIPPOR_TYPE_CLIPBOARD, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
    );

    obj_signals[SIGNAL_SELECTION] = g_signal_new_class_handler(
        "selection", G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS | G_SIGNAL_NO_RECURSE |
            G_SIGNAL_DETAILED,
        G_CALLBACK(selection_default_cb), NULL, NULL, NULL, G_TYPE_NONE, 1,
        G_TYPE_HASH_TABLE
    );
}

static void
wayland_seat_init(WaylandSeat *self)
{
    self->name = "";
}

static void
selection_default_cb(WaylandSeat *self G_GNUC_UNUSED, GHashTable *mime_types)
{
    if (mime_types != NULL)
        g_hash_table_unref(mime_types);
}

static void
on_removed_from_clipboard(
    GObject *object G_GNUC_UNUSED, GObject *client, gpointer data
)
{
    WaylandSeat *seat = data;

    if (seat->regular_cb == CLIPPOR_CLIPBOARD(client))
        g_clear_object(&seat->regular_cb);
    else if (seat->primary_cb == CLIPPOR_CLIPBOARD(client))
        g_clear_object(&seat->primary_cb);
}

static void
on_ct_dispose(gpointer data, GObject *object G_GNUC_UNUSED)
{
    WaylandSeat *seat = data;

    wayland_seat_destroy(seat);
}

static void wl_seat_listener_capabilities(
    void *data, struct wl_seat *seat, uint32_t capabilties
);

static void
wl_seat_listener_name(void *data, struct wl_seat *seat, const char *name);

struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_listener_capabilities, .name = wl_seat_listener_name
};

WaylandSeat *
wayland_seat_new(
    WaylandConnection *ct, struct wl_seat *seat_proxy, guint32 numerical_name,
    GError **error
)
{
    g_return_val_if_fail(wayland_connection_is_active(ct), NULL);
    g_return_val_if_fail(seat_proxy != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    WaylandSeat *seat = g_object_new(WAYLAND_TYPE_SEAT, "connection", ct, NULL);

    seat->proxy = seat_proxy;

    wl_seat_add_listener(seat->proxy, &wl_seat_listener, g_object_ref(seat));

    // Get name and capabilities
    if (!wayland_connection_roundtrip(ct, error))
    {
        g_prefix_error_literal(error, "Cannot get Wayland seat attributes");
        g_object_unref(seat);
        return NULL;
    }

    g_debug("New Wayland seat '%s'", seat->name);

    g_object_unref(seat);
    seat->numerical_name = numerical_name;

    return seat;
}

static void
wl_seat_listener_capabilities(
    void *data, struct wl_seat *seat G_GNUC_UNUSED, uint32_t capabilties
)
{
    WaylandSeat *obj = data;

    obj->capabilities = capabilties;
}

static void
wl_seat_listener_name(
    void *data, struct wl_seat *seat G_GNUC_UNUSED, const char *name
)
{
    WaylandSeat *obj = data;

    obj->name = g_strdup(name);
}

gchar *
wayland_seat_get_name(WaylandSeat *self)
{
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), NULL);

    return self->name;
}

guint32
wayland_seat_get_numerical_name(WaylandSeat *self)
{
    // Just give a warning message
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), self->numerical_name);

    return self->numerical_name;
}

struct wl_seat *
wayland_seat_get_proxy(WaylandSeat *self)
{
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), NULL);

    return self->proxy;
}

gboolean
wayland_seat_is_active(WaylandSeat *self)
{
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), FALSE);
    return self->active;
}

static void data_device_event_data_offer(
    void *data, WaylandDataDevice *device, WaylandDataOffer *offer
);
static void data_device_event_selection(
    void *data, WaylandDataDevice *device, WaylandDataOffer *offer,
    ClipporSelectionType selection
);
static void data_device_event_finished(void *data, WaylandDataDevice *device);

static void data_source_event_send(
    void *data, WaylandDataSource *source, const char *mime_type, int32_t fd
);
static void data_source_event_cancelled(void *data, WaylandDataSource *source);

static WaylandDataDeviceListener data_device_listener = {
    .data_offer = data_device_event_data_offer,
    .selection = data_device_event_selection,
    .finished = data_device_event_finished
};

static WaylandDataOfferListener data_offer_listener = {.offer = NULL};

static WaylandDataSourceListener data_source_listener = {
    .send = data_source_event_send, .cancelled = data_source_event_cancelled
};

/*
 * Start listening for new data offers
 */
static gboolean
wayland_seat_start(WaylandSeat *self, GError **error)
{
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), FALSE);
    g_return_val_if_fail(wayland_connection_is_active(self->ct), FALSE);
    g_return_val_if_fail(!wayland_seat_is_active(self), FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    self->data_device_manager =
        wayland_connection_get_data_device_manager(self->ct);

    if (self->data_device_manager == NULL)
    {
        g_set_error(
            error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_START,
            "No Wayland data control protocol is available"
        );
        return FALSE;
    }

    self->data_device = wayland_data_device_manager_get_data_device(
        self->data_device_manager, self
    );
    wayland_data_device_add_listener(
        self->data_device, &data_device_listener, self
    );

    // Let the main event loop dispatch the events later
    if (!wayland_connection_flush(self->ct, error))
    {
        g_assert(error == NULL || *error != NULL);
        g_prefix_error(error, "Failed starting seat '%s': ", self->name);
        return FALSE;
    }

    self->active = TRUE;

    return TRUE;
}

static void
wayland_seat_stop(WaylandSeat *self)
{
    g_return_if_fail(WAYLAND_IS_SEAT(self));
    g_return_if_fail(wayland_connection_is_active(self->ct));
    g_return_if_fail(wayland_seat_is_active(self));

    wayland_data_device_manager_unused(self->data_device_manager);
    wayland_data_device_destroy(self->data_device);
    wayland_data_source_destroy(self->data_source);

    self->active = FALSE;

    wayland_connection_flush(self->ct, NULL);
}

/*
 * Destroy the seat proxy, making the seat permanently inactive. Also removes it
 * from any clipboards.
 */
void
wayland_seat_destroy(WaylandSeat *self)
{
    g_return_if_fail(WAYLAND_IS_SEAT(self));

    wayland_seat_set_status(self, FALSE);
    wl_seat_destroy(self->proxy);
    self->proxy = NULL;

    g_object_set(self, "clipboard-regular", NULL, NULL);
    g_object_set(self, "clipboard-primary", NULL, NULL);
}

/*
 * Returns FALSE on error
 */
gboolean
wayland_seat_set_status(WaylandSeat *self, gboolean active)
{
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), FALSE);

    g_object_set(self, "active", active, NULL);

    if (active != self->active)
        return FALSE;
    return TRUE;
}

/*
 * Returns NULL on error
 */
static GBytes *
receive_data(int32_t fd, GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    GByteArray *data = g_byte_array_new();
    GPollFD pfd = {.fd = fd, .events = G_IO_IN};

    // Make pipe (read end) non-blocking
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == -1)
    {
        g_set_error(
            error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_RECEIVE,
            "fcntl() failed: %s", g_strerror(errno)
        );
        g_byte_array_unref(data);
        return NULL;
    }

    guint8 *bytes = g_malloc(8192);
    ssize_t r = 0;

    // Only poll before reading when we first start, then we do non-blocking
    // reads and check for EAGAIN or EINTR to signal to poll again.
    goto poll_data;

    while (errno = 0, TRUE)
    {
        r = read(fd, bytes, 8192);

        if (r == 0)
            break;
        else if (r < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
poll_data:
                // TODO: ADD CONFIGURABLE TIMEOUT
                if (g_poll(&pfd, 1, 3000) > 0)
                    continue;
                else
                    g_set_error(
                        error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_RECEIVE,
                        "g_poll() failed: %s", g_strerror(errno)
                    );
                break;
            }
            g_set_error(
                error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_RECEIVE,
                "read() failed: %s", g_strerror(errno)
            );
            break;
        }
        g_byte_array_append(data, bytes, r);
    }
    g_free(bytes);

    if (*error != NULL)
    {
        g_byte_array_unref(data);
        return NULL;
    }

    return g_byte_array_free_to_bytes(data);
}

static gboolean
send_data(int32_t fd, GBytes *data, GError **error)
{
    g_return_val_if_fail(data != NULL, FALSE);
    g_return_val_if_fail(fd >= 0, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    gsize length;
    const char *stuff = g_bytes_get_data(data, &length);

    GPollFD pfd = {.fd = fd, .events = G_IO_OUT};
    ssize_t written = 0;
    size_t total = 0;

    // TODO: ADD CONFIGURABLE TIMEOUT
    while (errno = 0, total < length && g_poll(&pfd, 1, 3000) > 0)
    {
        written = write(fd, stuff + total, length - total);

        if (written == -1)
        {
            g_set_error(
                error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_SEND,
                "write() failed: %s", g_strerror(errno)
            );
            break;
        }
        total += written;
    }

    if (errno != 0)
        g_set_error(
            error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_SEND,
            "g_poll() failed: %s", g_strerror(errno)
        );

    if (*error != NULL)
        return FALSE;

    return TRUE;
}

static gboolean
wayland_seat_set_selection(
    WaylandSeat *self, ClipporSelectionType selection, ClipporEntry *entry,
    GError **error
)
{
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), FALSE);
    g_return_val_if_fail(CLIPPOR_IS_ENTRY(entry), FALSE);
    g_return_val_if_fail(selection != CLIPPOR_SELECTION_TYPE_NONE, FALSE);
    g_return_val_if_fail(
        wayland_data_device_is_valid(self->data_device), FALSE
    );
    g_return_val_if_fail(
        wayland_data_device_manager_is_valid(self->data_device_manager), FALSE
    );

    // Don't need to worry about possibly leaking the old data source if it
    // exists because the cancelled event will be called for it.
    self->data_source = wayland_data_device_manager_create_data_source(
        self->data_device_manager
    );

    wayland_data_source_add_listener(
        self->data_source, &data_source_listener, entry
    );

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);
    GHashTableIter iter;
    const gchar *mime_type;

    g_hash_table_iter_init(&iter, mime_types);

    while (g_hash_table_iter_next(&iter, (gpointer *)&mime_type, NULL))
        wayland_data_source_offer(self->data_source, mime_type);

    wayland_data_device_set_seletion(
        self->data_device, self->data_source, selection
    );

    if (!wayland_connection_roundtrip(self->ct, error))
    {
        g_prefix_error_literal(error, "Failed setting selection");
        return FALSE;
    }

    return TRUE;
}

static void
wayland_seat_emit_selection_signal(
    WaylandSeat *self, ClipporSelectionType selection, GHashTable *mime_types
)
{
    g_return_if_fail(WAYLAND_IS_SEAT(self));
    g_return_if_fail(selection != CLIPPOR_SELECTION_TYPE_NONE);
    g_return_if_fail(mime_types != NULL);

    const gchar *detail;

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        detail = "regular";
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        detail = "primary";
    else
        return;

    g_signal_emit(
        self, obj_signals[SIGNAL_SELECTION], g_quark_from_static_string(detail),
        mime_types
    );
}

/*
 * Set the selections to the most recent repsective clipboard entry. If `force`
 * is TRUE then always set the selection even if the source client of the most
 * recent entry is the Wayland seat.
 */
static void
wayland_seat_update_selections(WaylandSeat *self, gboolean force)
{
    g_return_if_fail(WAYLAND_IS_SEAT(self));
    g_return_if_fail(self->data_device != NULL);

    ClipporClipboard *cbs[] = {self->regular_cb, self->primary_cb};
    ClipporSelectionType sels[] = {
        CLIPPOR_SELECTION_TYPE_REGULAR, CLIPPOR_SELECTION_TYPE_PRIMARY
    };

    for (gint i = 0; i < 2; i++)
    {
        GError *error = NULL;
        ClipporClipboard *cb = cbs[i];

        if (cb == NULL)
            continue;

        ClipporEntry *entry = clippor_clipboard_get_entry(cb, 0);

        if (entry == NULL)
            continue;

        GObject *source_client = clippor_entry_get_source(entry);

        if (!force && source_client == G_OBJECT(self))
            // No point in setting the selection if we are the source client
            continue;

        if (!wayland_seat_set_selection(self, sels[i], entry, &error))
        {
            g_info("Failed updating selection: %s", error->message);
            g_error_free(error);
        }
    }
}

static void
data_device_event_data_offer(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED, WaylandDataOffer *offer
)
{
    WaylandSeat *seat = data;

    if (seat->data_source != NULL)
        // We are the source client
        return;

    wayland_data_offer_add_listener(offer, &data_offer_listener, seat);
}

static void
data_device_event_selection(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED,
    WaylandDataOffer *offer, ClipporSelectionType selection
)
{
    WaylandSeat *seat = data;

    if (offer == NULL)
    {
        // Selection is cleared/empty. Don't emit a signal, instead set the
        // selection to the most recent history entry.
        wayland_seat_update_selections(seat, TRUE);

        return;
    }

    if (seat->data_source != NULL)
    {
        // We are the source client
        wayland_data_offer_destroy(offer);
        return;
    }

    // Each key is a mime type and its value is the data as a GBytes
    GPtrArray *mime_types = wayland_data_offer_get_mime_types(offer);
    GHashTable *table = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (void (*)(void *))g_bytes_unref
    );
    GError *error = NULL;

    // Receive data from each mime type
    for (guint i = 0; i < mime_types->len; i++)
    {
        const gchar *mime_type = mime_types->pdata[i];
        GBytes *data = NULL;
        ;

        int fds[2];

        if (pipe(fds) == -1)
        {
            g_set_error(
                &error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_RECEIVE,
                "pipe() failed: %s", g_strerror(errno)
            );
            break;
        }

        wayland_data_offer_receive(offer, mime_type, fds[1]);

        // Close our write end of the pipe so that we receive EOF.
        close(fds[1]);

        if (wayland_connection_flush(seat->ct, &error))
            data = receive_data(fds[0], &error);

        close(fds[0]);

        if (data == NULL)
        {
            g_assert(error != NULL);
            break;
        }

        g_hash_table_insert(table, g_strdup(mime_type), data);
    }

    if (error != NULL)
    {
        g_info("Data device selection event failed: %s", error->message);
        g_error_free(error);
        g_hash_table_unref(table);
    }
    else
        wayland_seat_emit_selection_signal(seat, selection, table);

    wayland_data_offer_destroy(offer);
}

static void
data_device_event_finished(void *data, WaylandDataDevice *device)
{
    WaylandSeat *seat = data;

    wayland_data_device_destroy(device);
    seat->data_device = NULL;
}

static void
data_source_event_send(
    void *data, WaylandDataSource *source G_GNUC_UNUSED, const char *mime_type,
    int32_t fd
)
{
    GError *error = NULL;
    ClipporEntry *entry = data;
    GBytes *stuff =
        g_hash_table_lookup(clippor_entry_get_mime_types(entry), mime_type);

    if (!send_data(fd, stuff, &error))
    {
        g_assert(error != NULL);
        g_info("Data source send event failed: %s", error->message);
        g_error_free(error);
    }

    close(fd);
}

static void
data_source_event_cancelled(void *data G_GNUC_UNUSED, WaylandDataSource *source)
{
    wayland_data_source_destroy(source);
}
