#pragma once

#include "clippor-client.h"
#include "wayland-connection.h"
#include <glib-object.h>
#include <wayland-client.h>

#define WAYLAND_SEAT_ERROR (wayland_seat_error_quark())

typedef enum
{
    WAYLAND_SEAT_ERROR_START,
    WAYLAND_SEAT_ERROR_RECEIVE,
    WAYLAND_SEAT_ERROR_SEND,
    WAYLAND_SEAT_ERROR_SET,
    WAYLAND_SEAT_ERROR_NO_DATA_PROTOCOL
} WaylandSeatError;

GQuark wayland_seat_error_quark(void);

#define WAYLAND_TYPE_SEAT (wayland_seat_get_type())

G_DECLARE_FINAL_TYPE(WaylandSeat, wayland_seat, WAYLAND, SEAT, ClipporClient)

WaylandSeat *wayland_seat_new(
    WaylandConnection *ct, struct wl_seat *seat_proxy, uint32_t numerical_name,
    GError **error
);

char *wayland_seat_get_name(WaylandSeat *self);
uint32_t wayland_seat_get_numerical_name(WaylandSeat *self);
struct wl_seat *wayland_seat_get_proxy(WaylandSeat *self);
