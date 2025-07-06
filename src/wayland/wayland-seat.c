#include "wayland-seat.h"
#include "util.h"
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
    WaylandSeat *parent;

    const char *name; // Set at construct time
    ClipporSelectionType type;

    GWeakRef entry; // Current entry for selection. We use a weak
                    // reference in case the entry is removed.

    // When there is a new offer, we remove the previous if any, save it
    // here, and only attempt to receive from it when requested
    WaylandDataOffer *offer;

    WaylandDataSource *source;
} WaylandSeatSelection;

struct _WaylandSeat
{
    ClipporClient parent;

    struct wl_seat *proxy;

    char *name;
    uint32_t numerical_name;
    enum wl_seat_capability capabilities;
    int data_timeout; // Timeout when waiting for data.

    WaylandConnection *ct; // Parent connection

    struct
    {
        WaylandDataDeviceManager *manager;
        WaylandDataDevice *device;

        WaylandSeatSelection regular;
        WaylandSeatSelection primary;
    } clipboard;
};

G_DEFINE_TYPE(WaylandSeat, wayland_seat, CLIPPOR_TYPE_CLIENT)

typedef enum
{
    PROP_DATA_TIMEOUT = 1,
    N_PROPERTIES
} WaylandSeatProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {NULL};

static WaylandSeatSelection *
wayland_seat_get_selection(WaylandSeat *self, ClipporSelectionType selection);

static gboolean wayland_seat_clipboard_setup(WaylandSeat *self, GError **error);
static void wayland_seat_clipboard_unsetup(WaylandSeat *self);

static gboolean wayland_seat_update_selection(
    WaylandSeat *self, ClipporSelectionType selection, gboolean ignore_if_none,
    GError **error
);

static GPtrArray *wayland_seat_client_get_mime_types(
    ClipporClient *self, ClipporSelectionType selection
);
static GBytes *wayland_seat_client_get_data(
    ClipporClient *self, const char *mime_type, ClipporSelectionType selection,
    GError **error
);
static gboolean wayland_seat_client_set_entry(
    ClipporClient *self, ClipporEntry *entry, ClipporSelectionType selection,
    GError **error
);

static void
wayland_seat_set_property(
    GObject *object, uint property_id, const GValue *value, GParamSpec *pspec
)
{
    WaylandSeat *self = WAYLAND_SEAT(object);

    switch ((WaylandSeatProperty)property_id)
    {
    case PROP_DATA_TIMEOUT:
        self->data_timeout = g_value_get_int(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
wayland_seat_get_property(
    GObject *object, uint property_id, GValue *value, GParamSpec *pspec
)
{
    WaylandSeat *self = WAYLAND_SEAT(object);

    switch ((WaylandSeatProperty)property_id)
    {
    case PROP_DATA_TIMEOUT:
        g_value_set_int(value, self->data_timeout);
        break;
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
    ClipporClientClass *clipporclient_class = CLIPPOR_CLIENT_CLASS(class);
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    clipporclient_class->get_data = wayland_seat_client_get_data;
    clipporclient_class->get_mime_types = wayland_seat_client_get_mime_types;
    clipporclient_class->set_entry = wayland_seat_client_set_entry;

    gobject_class->set_property = wayland_seat_set_property;
    gobject_class->get_property = wayland_seat_get_property;

    gobject_class->dispose = wayland_seat_dispose;
    gobject_class->finalize = wayland_seat_finalize;

    obj_properties[PROP_DATA_TIMEOUT] = g_param_spec_int(
        "data-timeout", "Data timeout", "Timeout to use when transferring data",
        -1, G_MAXINT, 500, G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
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

    self->clipboard.regular.parent = self;
    self->clipboard.primary.parent = self;

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
    WaylandConnection *ct, struct wl_seat *seat_proxy, uint32_t numerical_name,
    GError **error
)
{
    g_assert(seat_proxy != NULL);
    g_assert(error == NULL || *error == NULL);

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

    g_object_bind_property(
        ct, "data-timeout", seat, "data-timeout", G_BINDING_DEFAULT
    );

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

char *
wayland_seat_get_name(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return self->name;
}

uint32_t
wayland_seat_get_numerical_name(WaylandSeat *self)
{
    // Just give a warning message
    g_assert(WAYLAND_IS_SEAT(self));

    return self->numerical_name;
}

struct wl_seat *
wayland_seat_get_proxy(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

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
        sel->offer = NULL;

        // Set selection to latest entry (which should have the same data as the
        // previous offer before the selection cleared. This is unless there is
        // no entry then nothing is done.
        if (!wayland_seat_update_selection(seat, selection, TRUE, &error))
        {
            g_message(
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

    char *signal_name;

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        signal_name = "selection::regular";
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        signal_name = "selection::primary";
    else
        return;

    g_signal_emit_by_name(seat, signal_name, selection);
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
    WaylandSeatSelection *sel = data;
    ClipporEntry *entry = g_weak_ref_get(&sel->entry);

    GBytes *stuff = clippor_entry_get_data(entry, mime_type, &error);

    if (stuff == NULL)
    {
        if (error != NULL)
        {
            g_message(
                "Data source send event failed, cannot get '%s' data: %s",
                mime_type, error->message
            );
            g_error_free(error);
        }
        else
            // No entry for mime type exists in database
            g_message(
                "No entry exists for '%s' in database for entry id '%s'",
                mime_type, clippor_entry_get_id(entry)
            );

        goto exit;
    }

    if (!util_send_data(fd, stuff, sel->parent->data_timeout, &error))
    {
        g_assert(error != NULL);
        g_message("Data source send event failed: %s", error->message);
        g_error_free(error);
    }

exit:
    g_bytes_unref(stuff);
    g_object_unref(entry);

    close(fd);
}

static void
data_source_event_cancelled(void *data, WaylandDataSource *source)
{
    WaylandSeatSelection *sel = data;

    wayland_data_source_destroy(source);

    if (source == sel->source)
        sel->source = NULL;
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

    wayland_data_source_add_listener(sel->source, &data_source_listener, sel);

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);
    GHashTableIter iter;
    const char *mime_type;

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
 */
static GPtrArray *
wayland_seat_client_get_mime_types(
    ClipporClient *self, ClipporSelectionType selection
)
{
    g_assert(WAYLAND_IS_SEAT(self));

    WaylandSeat *seat = WAYLAND_SEAT(self);
    WaylandSeatSelection *sel = wayland_seat_get_selection(seat, selection);

    if (sel->offer == NULL)
        return NULL;

    return wayland_data_offer_get_mime_types(sel->offer);
}

static GBytes *
wayland_seat_client_get_data(
    ClipporClient *self, const char *mime_type, ClipporSelectionType selection,
    GError **error
)
{
    g_assert(WAYLAND_IS_SEAT(self));

    WaylandSeat *seat = WAYLAND_SEAT(self);
    WaylandSeatSelection *sel = wayland_seat_get_selection(seat, selection);
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

    if (wayland_connection_flush(seat->ct, error))
        data = util_receive_data(fds[0], seat->data_timeout, error);

    close(fds[0]);

    return data;
}

static gboolean
wayland_seat_client_set_entry(
    ClipporClient *self, ClipporEntry *entry, ClipporSelectionType selection,
    GError **error
)
{
    g_assert(WAYLAND_IS_SEAT(self));

    WaylandSeat *seat = WAYLAND_SEAT(self);
    WaylandSeatSelection *sel = wayland_seat_get_selection(seat, selection);

    g_weak_ref_set(&sel->entry, entry);

    if (!wayland_seat_update_selection(seat, sel->type, FALSE, error))
    {
        g_prefix_error(
            error, "Failed updating selection for seat '%s': ", seat->name
        );
        return FALSE;
    }
    return TRUE;
}
