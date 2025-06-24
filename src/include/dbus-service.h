#pragma once

#include "clippor-clipboard.h"
#include <gio/gio.h>
#include <glib.h>

gboolean dbus_service_init(GError **error);

void dbus_service_add_clipboard(ClipporClipboard *cb);
