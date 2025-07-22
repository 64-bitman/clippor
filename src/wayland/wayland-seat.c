#include "wayland-seat.h"
#include "clippor-selection.h"
#include "wayland-connection.h"
#include "wayland-selection.h"
#include <glib-object.h>
#include <glib.h>
#include <wayland-client.h>

G_DEFINE_QUARK(WAYLAND_SEAT_ERROR, wayland_seat_error)

struct _WaylandSeat
{
    GObject parent_instance;

    struct wl_seat *proxy;

    uint32_t numerical_name;
    char *name;
    uint32_t capabilities;

    int data_timeout;

    // We listen to the device and send out signals when there is a new
    // selection. Let the individual selection objects handle what to do.
    WaylandDataDeviceManager *manager;
    WaylandDataDevice *device;

    WaylandConnection *ct; // Don't create a new reference, it will always
                           // outlive us anyways.

    WaylandSelection *regular;
    WaylandSelection *primary;

    gboolean active;
};

G_DEFINE_TYPE(WaylandSeat, wayland_seat, G_TYPE_OBJECT)

typedef enum
{
    PROP_DATA_TIMEOUT = 1,
    N_PROPERTIES
} WaylandSeatProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {NULL};

static void
wayland_seat_set_property(
    GObject *object, guint property_id, const GValue *value, GParamSpec *pspec
)
{
    WaylandSeat *self = WAYLAND_SEAT(object);

    switch (property_id)
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
    GObject *object, guint property_id, GValue *value, GParamSpec *pspec
)
{
    WaylandSeat *self = WAYLAND_SEAT(object);

    switch (property_id)
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

    wayland_seat_make_inert(self);
    g_free(self->name);

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

    // Will be binded to parent connection property
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
    // Initialize selections
    self->regular = wayland_selection_new(self, CLIPPOR_SELECTION_TYPE_REGULAR);
    self->primary = wayland_selection_new(self, CLIPPOR_SELECTION_TYPE_PRIMARY);
}

static void
wl_seat_listener_event_name(
    void *data, struct wl_seat *proxy G_GNUC_UNUSED, const char *name
)
{
    ((WaylandSeat *)data)->name = g_strdup(name);
}

static void
wl_seat_listener_event_capabilities(
    void *data, struct wl_seat *proxy G_GNUC_UNUSED, uint32_t capabilities
)
{
    ((WaylandSeat *)data)->capabilities = capabilities;
}

static void data_device_listener_event_data_offer(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED, WaylandDataOffer *offer
);
static void data_device_listener_event_selection(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED,
    WaylandDataOffer *offer, ClipporSelectionType selection
);
static void
data_device_listener_event_finished(void *data, WaylandDataDevice *device);

static const WaylandDataDeviceListener data_device_listener = {
    .data_offer = data_device_listener_event_data_offer,
    .selection = data_device_listener_event_selection,
    .finished = data_device_listener_event_finished
};

static const struct wl_seat_listener wl_seat_listener = {
    .name = wl_seat_listener_event_name,
    .capabilities = wl_seat_listener_event_capabilities
};

WaylandSeat *
wayland_seat_new(
    WaylandConnection *ct, struct wl_seat *proxy, uint32_t numerical_name,
    GError **error
)
{
    g_assert(WAYLAND_IS_CONNECTION(ct));
    g_assert(proxy != NULL);
    g_assert(error == NULL || *error == NULL);

    WaylandSeat *seat = g_object_new(WAYLAND_TYPE_SEAT, NULL);

    seat->ct = ct;
    seat->proxy = proxy;
    seat->numerical_name = numerical_name;
    seat->active = TRUE;

    // Get name and capabilities
    wl_seat_add_listener(proxy, &wl_seat_listener, seat);

    if (!wayland_connection_roundtrip(ct, error))
        goto fail;

    // Starting listenining for data events
    seat->manager = wayland_connection_get_data_device_manager(ct);

    if (seat->manager == NULL)
    {
        // No data protocol available
        g_set_error(
            error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_NO_DATA_PROTOCOL,
            "No data protocol available"
        );
        goto fail;
    }

    seat->device =
        wayland_data_device_manager_get_data_device(seat->manager, seat);

    wayland_data_device_add_listener(seat->device, &data_device_listener, seat);

    // Let the event loop do the display flushing and dispatching. This gives us
    // time to have our selections be added to clipboards without missing out on
    // the initial selection signal.

    // Not sure if this is supposed to happen...
    if (seat->name == NULL)
    {
        g_set_error_literal(
            error, WAYLAND_SEAT_ERROR, WAYLAND_SEAT_ERROR_CREATE,
            "Seat name is NULL"
        );
        goto fail;
    }

    g_object_bind_property(
        ct, "data-timeout", seat, "data-timeout", G_BINDING_DEFAULT
    );

    return seat;
fail:
    g_object_unref(seat);
    return NULL;
}

static gboolean
data_offer_listener_event_offer(
    void *data G_GNUC_UNUSED, WaylandDataOffer *offer G_GNUC_UNUSED,
    const char *mime_type G_GNUC_UNUSED
)
{
    return TRUE;
}

static const WaylandDataOfferListener data_offer_listener = {
    .offer = data_offer_listener_event_offer
};

static void
data_device_listener_event_data_offer(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED, WaylandDataOffer *offer
)
{
    WaylandSeat *seat = data;

    wayland_data_offer_add_listener(offer, &data_offer_listener, seat);
}

/*
 * Don't do anything, let the individual selections do the stuff.
 */
static void
data_device_listener_event_selection(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED,
    WaylandDataOffer *offer, ClipporSelectionType selection
)
{
    WaylandSeat *seat = data;
    WaylandSelection *wsel = wayland_seat_get_selection(seat, selection);

    wayland_selection_new_offer(wsel, offer);
}

static void
data_device_listener_event_finished(void *data, WaylandDataDevice *device)
{
    WaylandSeat *seat = data;
    wayland_data_device_destroy(device);

    seat->device =
        wayland_data_device_manager_get_data_device(seat->manager, seat);

    wayland_data_device_add_listener(seat->device, &data_device_listener, seat);
}

/*
 * Makes the seat inert, meaning it won't emit any signals and any calls on it
 * will be ignored or return an old value/error. Cannot be undone.
 */
void
wayland_seat_make_inert(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    if (!self->active)
        return;

    g_clear_pointer(&self->regular, wayland_selection_unref_and_inert);
    g_clear_pointer(&self->primary, wayland_selection_unref_and_inert);

    g_clear_pointer(&self->proxy, wl_seat_destroy);
    g_clear_pointer(&self->manager, wayland_data_device_manager_discard);
    g_clear_pointer(&self->device, wayland_data_device_destroy);

    self->active = FALSE;
}

void
wayland_seat_unref_and_inert(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    wayland_seat_make_inert(self);
    g_object_unref(self);
}

const char *
wayland_seat_get_name(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return self->name;
}

uint32_t
wayland_seat_get_numerical_name(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return self->numerical_name;
}

struct wl_seat *
wayland_seat_get_proxy(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return self->proxy;
}

gboolean
wayland_seat_is_active(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return self->active;
}

WaylandConnection *
wayland_seat_get_connection(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return self->ct;
}

WaylandSelection *
wayland_seat_get_selection(WaylandSeat *self, ClipporSelectionType selection)
{
    g_assert(WAYLAND_IS_SEAT(self));
    g_assert(selection != CLIPPOR_SELECTION_TYPE_NONE);

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        return self->regular;
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        return self->primary;
    else
        // Shouldn't happen
        return NULL;
}

WaylandDataDeviceManager *
wayland_seat_get_data_device_manager(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return self->manager;
}

WaylandDataDevice *
wayland_seat_get_data_device(WaylandSeat *self)
{
    g_assert(WAYLAND_IS_SEAT(self));

    return self->device;
}
