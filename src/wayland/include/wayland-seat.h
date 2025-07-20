#pragma once

#include <glib-object.h>
#include <glib.h>
#include <wayland-client.h>

G_DECLARE_FINAL_TYPE(WaylandSeat, wayland_seat, WAYLAND, SEAT, GObject)
#define WAYLAND_TYPE_SEAT (wayland_seat_get_type())
