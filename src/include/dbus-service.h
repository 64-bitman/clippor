#pragma once

#include "clippor-clipboard.h"
#include "wayland-connection.h"
#include <gio/gio.h>
#include <glib.h>

gboolean dbus_service_init(int timeout, GError **error);
void dbus_server_uninit(void);

void dbus_service_add_clipboard(ClipporClipboard *cb);
void dbus_service_remove_clipboard(ClipporClipboard *cb);

void dbus_service_add_wayland_connection(WaylandConnection *ct);
void dbus_service_remove_wayland_connection(WaylandConnection *ct);
