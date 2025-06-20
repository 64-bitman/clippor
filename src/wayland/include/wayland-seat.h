#pragma once

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
} WaylandSeatError;

GQuark wayland_seat_error_quark(void);

#define WAYLAND_TYPE_SEAT (wayland_seat_get_type())

G_DECLARE_FINAL_TYPE(WaylandSeat, wayland_seat, WAYLAND, SEAT, GObject)

WaylandSeat *wayland_seat_new(
    WaylandConnection *ct, struct wl_seat *seat_proxy, guint32 numerical_name,
    GError **error
);

gchar *wayland_seat_get_name(WaylandSeat *self);
guint32 wayland_seat_get_numerical_name(WaylandSeat *self);
struct wl_seat *wayland_seat_get_proxy(WaylandSeat *self);

GPtrArray *wayland_seat_clipboard_get_mime_types(
    WaylandSeat *self, ClipporSelectionType selection
);
GBytes *wayland_seat_clipboard_receive_data(
    WaylandSeat *self, ClipporSelectionType selection, const char *mime_type,
    GError **error
);
