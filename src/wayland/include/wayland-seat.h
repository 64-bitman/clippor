#pragma once

#include "clippor-selection.h"
#include <glib-object.h>
#include <glib.h>
#include <wayland-client.h>

G_DECLARE_FINAL_TYPE(WaylandSeat, wayland_seat, WAYLAND, SEAT, GObject)
#define WAYLAND_TYPE_SEAT (wayland_seat_get_type())

typedef struct _WaylandConnection WaylandConnection;
typedef struct _WaylandSelection WaylandSelection;
typedef struct WaylandDataDeviceManager WaylandDataDeviceManager;
typedef struct WaylandDataDevice WaylandDataDevice;

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
WaylandConnection *wayland_seat_get_connection(WaylandSeat *self);
WaylandSelection *
wayland_seat_get_selection(WaylandSeat *self, ClipporSelectionType selection);
WaylandDataDeviceManager *
wayland_seat_get_data_device_manager(WaylandSeat *self);
WaylandDataDevice *wayland_seat_get_data_device(WaylandSeat *self);
