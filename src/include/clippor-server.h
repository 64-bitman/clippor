#pragma once

#include "clippor-config.h"
#include "clippor-database.h"
#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(ClipporServer, clippor_server, CLIPPOR, SERVER, GObject)
#define CLIPPOR_TYPE_SERVER (clippor_server_get_type())

ClipporServer *clippor_server_new(ClipporConfig *cfg, ClipporDatabase *db);

gboolean clippor_server_start(ClipporServer *self, GError **error);
