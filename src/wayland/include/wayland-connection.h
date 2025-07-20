#pragma once

#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(
    WaylandConnection, wayland_connection, WAYLAND, CONNECTION, GObject
)
#define WAYLAND_TYPE_CONNECTION (wayland_connection_get_type())

typedef enum
{
    WAYLAND_CONNECTION_ERROR_CONNECT,
    WAYLAND_CONNECTION_ERROR_FLUSH,
    WAYLAND_CONNECTION_ERROR_DISPATCH,
    WAYLAND_CONNECTION_ERROR_ROUNDTRIP
} WaylandConnectionError;

#define WAYLAND_CONNECTION_ERROR (wayland_connection_error_quark())
GQuark wayland_connection_error_quark(void);

WaylandConnection *wayland_connection_new(const char *display);

gboolean wayland_connection_start(WaylandConnection *self, GError **error);
void wayland_connection_stop(WaylandConnection *self);

int wayland_connection_get_fd(WaylandConnection *self);

gboolean wayland_connection_flush(WaylandConnection *self, GError **error);
int wayland_connection_dispatch(WaylandConnection *self, GError **error);
gboolean wayland_connection_roundtrip(WaylandConnection *self, GError **error);
