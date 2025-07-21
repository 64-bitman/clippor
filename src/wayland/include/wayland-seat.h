#pragma once

#include <glib-object.h>
#include <glib.h>
#include <wayland-client.h>

G_DECLARE_FINAL_TYPE(WaylandSeat, wayland_seat, WAYLAND, SEAT, GObject)
#define WAYLAND_TYPE_SEAT (wayland_seat_get_type())

typedef enum
{
    WAYLAND_SEAT_ERROR_CREATE,
    WAYLAND_SEAT_ERROR_NO_DATA_PROTOCOL
} WaylandSeatError;

#define WAYLAND_SEAT_ERROR (wayland_seat_error_quark())
GQuark wayland_seat_error_quark(void);

typedef struct _WaylandConnection WaylandConnection;

WaylandSeat *wayland_seat_new(
    WaylandConnection *ct, struct wl_seat *proxy, uint32_t numerical_name,
    GError **error
);

void wayland_seat_make_inert(WaylandSeat *self);
void wayland_seat_unref_and_inert(WaylandSeat *self);

const char *wayland_seat_get_name(WaylandSeat *self);
uint32_t wayland_seat_get_numerical_name(WaylandSeat *self);
struct wl_seat *wayland_seat_get_proxy(WaylandSeat *self);
gboolean wayland_seat_is_active(WaylandSeat *self);
