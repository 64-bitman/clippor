#include "wayland-seat.h"
#include "wayland-connection.h"
#include <glib-object.h>
#include <glib.h>
#include <wayland-client.h>

struct _WaylandSeat
{
    struct wl_seat *proxy;

    uint32_t numerical_name;
    char *name;
    uint32_t capabilities;

    WaylandConnection *ct; // Don't create a new reference, it will always
                           // outlive us anyways.
};

G_DEFINE_TYPE(WaylandSeat, wayland_seat, G_TYPE_OBJECT)

static void
wayland_seat_class_init(WaylandSeatClass *class)
{
}

static void
wayland_seat_init(WaylandSeat *self)
{
}
