#include "wayland-seat.h"
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

// WaylandSeat should always be weak referenced, with the only strong reference
// being its WaylandConnection that created it.

typedef struct
{
    // Set at construct time
    const gchar *name;
    ClipporSelectionType type;

    GWeakRef entry; // Current entry for selection. We use a weak
                    // reference in case the entry is removed.

    // When there is a new offer, we remove the previous if any, save it
    // here, and only attempt to receive from it when requests
    WaylandDataOffer *offer;

    WaylandDataSource *source;
} WaylandSeatSelection;

struct _WaylandSeat
{
    GObject parent;

    struct wl_seat *proxy;

    gchar *name;
    guint32 numerical_name;
    enum wl_seat_capability capabilities;
    gint timeout; // Timeout when waiting for data.

    WaylandConnection *ct; // Parent connection

    struct
    {
        WaylandDataDeviceManager *manager;
        WaylandDataDevice *device;

        WaylandSeatSelection regular;
        WaylandSeatSelection primary;
    } clipboard;
};

G_DEFINE_TYPE(WaylandSeat, wayland_seat, G_TYPE_OBJECT)

typedef enum
{
    PROP_NAME = 1,
    PROP_REGULAR_ENTRY,
    PROP_PRIMARY_ENTRY,
    PROP_SEND_DATA, // A "button" used to emit a signal with the all the data
                    // and mime types of the current offer for the selection
                    // that it was set to. This is in the form of a hash table.
    N_PROPERTIES
} WaylandSeatProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {
    NULL,
};

typedef enum
{
    SIGNAL_SELECTION,
    SIGNAL_SEND_DATA,
    N_SIGNALS
} WaylandSeatSignal;

static guint obj_signals[N_SIGNALS] = {0};

static void
default_handler_send_data_signal(GObject *object, GHashTable *mime_types);

static WaylandSeatSelection *
wayland_seat_get_selection(WaylandSeat *self, ClipporSelectionType selection);

static gboolean wayland_seat_clipboard_setup(WaylandSeat *self, GError **error);
static void wayland_seat_clipboard_unsetup(WaylandSeat *self);

static GBytes *receive_data(int32_t fd, gint timeout, GError **error);
static gboolean
send_data(int32_t fd, GBytes *data, gint timeout, GError **error);

static gboolean wayland_seat_update_selection(
    WaylandSeat *self, ClipporSelectionType selection, gboolean ignore_if_none,
    GError **error
);

static void
wayland_seat_set_property(
    GObject *object, guint property_id, const GValue *value, GParamSpec *pspec
)
{
    WaylandSeat *self = WAYLAND_SEAT(object);
    GError *error = NULL;
    WaylandSeatSelection *sel;

    switch ((WaylandSeatProperty)property_id)
    {
    case PROP_PRIMARY_ENTRY:
        sel = &self->clipboard.primary;
        goto new_entry;
    case PROP_REGULAR_ENTRY:
        sel = &self->clipboard.regular;
new_entry:;
        GError *error = NULL;
        ClipporEntry *entry = g_value_get_object(value);

        g_weak_ref_set(&sel->entry, entry);

        //  Don't want to immediately steal the selection when there is a new
        //  one.
        if (clippor_entry_is_from(entry) == object)
            break;

        if (!wayland_seat_update_selection(self, sel->type, FALSE, &error))
        {
            g_info(
                "Failed updating selection for seat '%s': %s", self->name,
                error->message
            );
            g_error_free(error);
        }

        break;
    case PROP_SEND_DATA:
    {
        ClipporSelectionType selection = g_value_get_enum(value);
        WaylandSeatSelection *sel = wayland_seat_get_selection(self, selection);

        GPtrArray *mime_types =
            wayland_seat_clipboard_get_mime_types(self, selection);

        if (mime_types == NULL)
            break;

        GHashTable *table = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, (void (*)(void *))g_bytes_unref
        );
        GError *error = NULL;

        // Get data for all mime types. If an error occurs, then don't emit a
        // signal.
        for (guint i = 0; i < mime_types->len; i++)
        {
            const gchar *mime_type = mime_types->pdata[i];

            GBytes *data = wayland_seat_clipboard_receive_data(
                self, selection, mime_type, &error
            );

            if (data == NULL)
            {
                g_assert(error != NULL);
                break;
            }

            g_hash_table_insert(table, g_strdup(mime_type), data);
        }

        g_ptr_array_unref(mime_types);

        if (error != NULL)
        {
            g_info(
                "Failed receiving data for Wayland seat '%s': %s", self->name,
                error->message
            );
            g_error_free(error);
            g_hash_table_unref(table);
            break;
        }
        g_signal_emit(
            self, obj_signals[SIGNAL_SEND_DATA],
            g_quark_from_static_string(sel->name), table
        );
        break;
    }
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
    case PROP_REGULAR_ENTRY:
    {
        ClipporEntry *entry = g_weak_ref_get(&self->clipboard.primary.entry);

        g_object_unref(entry); // Remove strong reference that g_weak_ref_get
                               // creates, because g_object_set_object already
                               // does that.
        g_value_set_object(value, entry);
        break;
    }
    case PROP_PRIMARY_ENTRY:
    {
        ClipporEntry *entry = g_weak_ref_get(&self->clipboard.primary.entry);

        g_object_unref(entry);
        g_value_set_object(value, entry);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
wayland_seat_dispose(GObject *object)
{
    G_OBJECT_CLASS(wayland_seat_parent_class)->dispose(object);
}

static void
wayland_seat_finalize(GObject *object)
{
    WaylandSeat *self = WAYLAND_SEAT(object);

    g_free(self->name);

    if (self->proxy != NULL)
        wl_seat_destroy(self->proxy);

    wayland_seat_clipboard_unsetup(self);

    g_weak_ref_clear(&self->clipboard.regular.entry);
    g_weak_ref_clear(&self->clipboard.primary.entry);

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
        "name", "Name", "Name of Wayland seat", "", G_PARAM_READABLE
    );
    obj_properties[PROP_REGULAR_ENTRY] = g_param_spec_object(
        "regular-entry", "Regular selection entry",
        "Entry for regular selection", CLIPPOR_TYPE_ENTRY, G_PARAM_READWRITE
    );
    obj_properties[PROP_PRIMARY_ENTRY] = g_param_spec_object(
        "primary-entry", "Primary selection entry",
        "Entry for primary selection", CLIPPOR_TYPE_ENTRY, G_PARAM_READWRITE
    );
    obj_properties[PROP_SEND_DATA] = g_param_spec_enum(
        "send-data", "Send data",
        "Emit a signal containing the data and mime types of the current offer "
        "for the set selection",
        CLIPPOR_TYPE_SELECTION_TYPE, CLIPPOR_SELECTION_TYPE_NONE,
        G_PARAM_WRITABLE
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
    );

    // Used to notify any clipboards that we have a new selection now
    obj_signals[SIGNAL_SELECTION] = g_signal_new(
        "selection", G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS | G_SIGNAL_NO_RECURSE |
            G_SIGNAL_DETAILED,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, CLIPPOR_TYPE_SELECTION_TYPE
    );
    obj_signals[SIGNAL_SEND_DATA] = g_signal_new_class_handler(
        "send-data", G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS | G_SIGNAL_NO_RECURSE |
            G_SIGNAL_DETAILED,
        G_CALLBACK(default_handler_send_data_signal), NULL, NULL, NULL,
        G_TYPE_NONE, 1, G_TYPE_HASH_TABLE
    );
}

static void
wayland_seat_init(WaylandSeat *self)
{
    self->name = "";

    self->clipboard.regular.name = "regular";
    self->clipboard.primary.name = "primary";

    self->clipboard.regular.type = CLIPPOR_SELECTION_TYPE_REGULAR;
    self->clipboard.primary.type = CLIPPOR_SELECTION_TYPE_PRIMARY;

    g_weak_ref_init(&self->clipboard.regular.entry, NULL);
    g_weak_ref_init(&self->clipboard.primary.entry, NULL);
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
    g_return_val_if_fail(seat_proxy != NULL, NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);

    WaylandSeat *seat = g_object_new(WAYLAND_TYPE_SEAT, NULL);

    seat->proxy = seat_proxy;
    seat->ct = ct; // No need to create new reference, ct will always outlive us

    wl_seat_add_listener(seat->proxy, &wl_seat_listener, seat);

    // Get name and capabilities
    if (!wayland_connection_roundtrip(ct, error) ||
        !wayland_seat_clipboard_setup(seat, error))
    {
        g_assert(error != NULL);
        g_prefix_error_literal(error, "Wayland seat failed: ");
        g_object_unref(seat);
        return NULL;
    }
    seat->numerical_name = numerical_name;

    return seat;
}

static void
default_handler_send_data_signal(
    GObject *object G_GNUC_UNUSED, GHashTable *mime_types
)
{
    g_hash_table_unref(mime_types);
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

static gboolean
wayland_seat_clipboard_valid(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return wayland_data_device_manager_is_valid(self->clipboard.manager) &&
           wayland_data_device_is_valid(self->clipboard.device);
}

static WaylandSeatSelection *
wayland_seat_get_selection(WaylandSeat *self, ClipporSelectionType selection)
{
    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        return &self->clipboard.regular;
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        return &self->clipboard.primary;
    else
    {
        g_assert(selection != CLIPPOR_SELECTION_TYPE_NONE);
        return NULL;
    }
}

static void wayland_data_device_listener_data_offer(
    void *data, WaylandDataDevice *device, WaylandDataOffer *offer
);
static void wayland_data_device_listener_selection(
    void *data, WaylandDataDevice *device, WaylandDataOffer *offer,
    ClipporSelectionType selection
);
static void
wayland_data_device_listener_finished(void *data, WaylandDataDevice *device);

static gboolean wayland_data_offer_listener_offer(
    void *data, WaylandDataOffer *offer, const char *mime_type
);

static void data_source_event_send(
    void *data, WaylandDataSource *source, const char *mime_type, int32_t fd
);
static void data_source_event_cancelled(void *data, WaylandDataSource *source);

static WaylandDataDeviceListener wayland_data_device_listener = {
    .data_offer = wayland_data_device_listener_data_offer,
    .selection = wayland_data_device_listener_selection,
    .finished = wayland_data_device_listener_finished
};

static WaylandDataOfferListener wayland_data_offer_listener = {
    .offer = wayland_data_offer_listener_offer
};

static WaylandDataSourceListener data_source_listener = {
    .send = data_source_event_send, .cancelled = data_source_event_cancelled
};

static gboolean
wayland_seat_clipboard_setup(WaylandSeat *self, GError **error)
{
    g_assert(WAYLAND_IS_SEAT(self));
    g_assert(error == NULL || *error == NULL);

    self->clipboard.manager =
        wayland_connection_get_data_device_manager(self->ct);
    self->clipboard.device = wayland_data_device_manager_get_data_device(
        self->clipboard.manager, self
    );

    wayland_data_device_add_listener(
        self->clipboard.device, &wayland_data_device_listener, self
    );

    // Let main loop dispatch events later
    if (!wayland_connection_flush(self->ct, error))
    {
        g_assert(error != NULL);
        g_prefix_error(
            error, "Failed setting up Wayland seat '%s' clipboard: ", self->name
        );
        wayland_seat_clipboard_unsetup(self);
        return FALSE;
    }

    return TRUE;
}

static void
wayland_seat_clipboard_unsetup(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    wayland_data_device_manager_unused(self->clipboard.manager);
    wayland_data_device_destroy(self->clipboard.device);

    wayland_data_source_destroy(self->clipboard.regular.source);
    wayland_data_source_destroy(self->clipboard.primary.source);

    wayland_data_offer_destroy(self->clipboard.regular.offer);
    wayland_data_offer_destroy(self->clipboard.primary.offer);
}

// Data offer event will not be sent if selection is cleared/empty.
static void
wayland_data_device_listener_data_offer(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED, WaylandDataOffer *offer
)
{
    wayland_data_offer_add_listener(offer, &wayland_data_offer_listener, data);
}

static void
wayland_data_device_listener_selection(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED,
    WaylandDataOffer *offer, ClipporSelectionType selection
)
{
    WaylandSeat *seat = data;
    WaylandSeatSelection *sel = wayland_seat_get_selection(seat, selection);

    // Destroy previous offer
    wayland_data_offer_destroy(sel->offer);

    if (offer == NULL)
    {
        GError *error = NULL;

        // Selection cleared/empty.
        sel->source = NULL;
        sel->offer = NULL;

        // Set selection to latest entry (which should have the same data as the
        // previous offer before the selection cleared. This is unless there is
        // no entry then nothing is done.
        if (!wayland_seat_update_selection(seat, selection, TRUE, &error))
        {
            g_info(
                "Failed updating selection for Wayland seat '%s': %s",
                seat->name, error->message
            );
            g_error_free(error);
        }
        return;
    }

    if (sel->source != NULL)
    {
        // We are the source client
        wayland_data_offer_destroy(offer);
        sel->offer = NULL;
        return;
    }

    sel->offer = offer;

    g_signal_emit(
        seat, obj_signals[SIGNAL_SELECTION],
        g_quark_from_static_string(sel->name), selection
    );
}

static void
wayland_data_device_listener_finished(void *data, WaylandDataDevice *device)
{
    WaylandSeat *seat = data;

    wayland_data_device_destroy(device);
    seat->clipboard.device = NULL;
}

static gboolean
wayland_data_offer_listener_offer(
    void *data, WaylandDataOffer *offer, const char *mime_type
)
{
    // Temporary
    (void)data;
    (void)offer;
    (void)mime_type;
    return TRUE;
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

    if (!send_data(fd, stuff, 3000, &error))
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
    // No need to set to NULL in WaylandSeat because we will receive selection
    // event and set source to NULL.
}

/*
 * Returns NULL on error
 */
static GBytes *
receive_data(int32_t fd, gint timeout, GError **error)
{
    g_assert(error == NULL || *error == NULL);
    g_assert(fd >= 0);

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

    GError *err = NULL;
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
                if (g_poll(&pfd, 1, timeout) > 0)
                    continue;
                else
                    g_set_error(
                        &err, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_RECEIVE,
                        "g_poll() failed: %s", g_strerror(errno)
                    );
                break;
            }
            g_set_error(
                &err, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_RECEIVE,
                "read() failed: %s", g_strerror(errno)
            );
            break;
        }
        g_byte_array_append(data, bytes, r);
    }
    g_free(bytes);

    if (err != NULL)
    {
        g_propagate_error(error, err);
        g_byte_array_unref(data);
        return NULL;
    }

    return g_byte_array_free_to_bytes(data);
}

static gboolean
send_data(int32_t fd, GBytes *data, gint timeout, GError **error)
{
    g_assert(data != NULL);
    g_assert(fd >= 0);
    g_assert(error == NULL || *error == NULL);

    gsize length;
    const char *stuff = g_bytes_get_data(data, &length);

    GPollFD pfd = {.fd = fd, .events = G_IO_OUT};
    ssize_t written = 0;
    size_t total = 0;

    while (errno = 0, total < length && g_poll(&pfd, 1, timeout) > 0)
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

/*
 * Set selection to the current entry. If `ignore_if_none` is TRUE, then if
 * there is no entry found, don't set the selection to NULL. This avoids
 * recursion loops when trying to update the seletion when the selection is
 * cleared.
 */
static gboolean
wayland_seat_update_selection(
    WaylandSeat *self, ClipporSelectionType selection, gboolean ignore_if_none,
    GError **error
)
{
    g_assert(WAYLAND_IS_SEAT(self));
    g_assert(error == NULL || *error == NULL);
    g_assert(wayland_seat_clipboard_valid(self));
    g_assert(selection != CLIPPOR_SELECTION_TYPE_NONE);

    WaylandSeatSelection *sel = wayland_seat_get_selection(self, selection);
    ClipporEntry *entry = g_weak_ref_get(&sel->entry);


    if (entry == NULL)
    {
        if (ignore_if_none)
            return TRUE;
        // Clear selection
        wayland_data_device_set_seletion(
            self->clipboard.device, NULL, selection
        );
        goto roundtrip;
    }

    wayland_data_source_destroy(sel->source);
    sel->source =
        wayland_data_device_manager_create_data_source(self->clipboard.manager);

    wayland_data_source_add_listener(sel->source, &data_source_listener, entry);

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);
    GHashTableIter iter;
    const gchar *mime_type;

    g_object_unref(entry);
    g_hash_table_iter_init(&iter, mime_types);

    while (g_hash_table_iter_next(&iter, (gpointer *)&mime_type, NULL))
        wayland_data_source_offer(sel->source, mime_type);

    wayland_data_device_set_seletion(
        self->clipboard.device, sel->source, selection
    );

roundtrip:
    if (!wayland_connection_roundtrip(self->ct, error))
    {
        g_prefix_error_literal(error, "Failed setting selection: ");
        return FALSE;
    }

    return TRUE;
}

/*
 * Get the mime types for the current offer or NULL if there is no offer.
 * Creates a new reference.
 */
GPtrArray *
wayland_seat_clipboard_get_mime_types(
    WaylandSeat *self, ClipporSelectionType selection
)
{
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), NULL);
    g_return_val_if_fail(selection != CLIPPOR_SELECTION_TYPE_NONE, NULL);

    WaylandSeatSelection *sel = wayland_seat_get_selection(self, selection);

    if (sel->offer == NULL)
        return NULL;

    return g_ptr_array_ref(wayland_data_offer_get_mime_types(sel->offer));
}

/*
 * Get data of current offer for selection. Returns NULL on error or if there is
 * no offer available, such as when the selection is empty.
 */
GBytes *
wayland_seat_clipboard_receive_data(
    WaylandSeat *self, ClipporSelectionType selection, const char *mime_type,
    GError **error
)
{
    g_return_val_if_fail(WAYLAND_IS_SEAT(self), NULL);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    g_return_val_if_fail(selection != CLIPPOR_SELECTION_TYPE_NONE, NULL);

    WaylandSeatSelection *sel = wayland_seat_get_selection(self, selection);
    GBytes *data = NULL;

    if (sel->offer == NULL)
        return NULL;

    int fds[2];

    if (pipe(fds) == -1)
    {
        g_set_error(
            error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_RECEIVE,
            "pipe() failed: %s", g_strerror(errno)
        );
        return NULL;
    }

    wayland_data_offer_receive(sel->offer, mime_type, fds[1]);

    // Close our write end of the pipe so that we receive EOF.
    close(fds[1]);

    if (wayland_connection_flush(self->ct, error))
        data = receive_data(fds[0], 3000, error);

    close(fds[0]);

    return data;
}
