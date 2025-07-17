#pragma once

#include "clippor-clipboard.h"
#include <gio/gio.h>
#include <glib.h>

gboolean dbus_service_init(int timeout, GError **error);
void dbus_server_uninit(void);

void dbus_service_add_clipboard(ClipporClipboard *cb);
void dbus_service_remove_clipboard(ClipporClipboard *cb);
