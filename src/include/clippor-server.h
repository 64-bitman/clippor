#pragma once

#include "clippor-config.h"
#include "clippor-database.h"
#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(ClipporServer, clippor_server, CLIPPOR, SERVER, GObject)
#define CLIPPOR_TYPE_SERVER (clippor_server_get_type())

typedef enum
{
    SERVER_ERROR_FAILED,
    SERVER_ERROR_CLIPBOARD_EXISTS
} ServerError;

#define SERVER_ERROR (server_error_quark())
GQuark server_error_quark(void);

ClipporServer *clippor_server_new(ClipporConfig *cfg, ClipporDatabase *db);

gboolean clippor_server_start(ClipporServer *self, GError **error);
