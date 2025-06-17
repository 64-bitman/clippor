#pragma once

#include "clippor-clipboard.h"
#include "wayland-connection.h"
#include <gio/gio.h>
#include <glib.h>

typedef struct
{
    GMainContext *context;
    GMainLoop *loop;
    GSettings *settings;

    // Temporary
    ClipporClipboard *cb;
    WaylandConnection *ct;
    WaylandSeat *seat;
} ClipporServer;

ClipporServer *start_server(void);
void stop_server(ClipporServer *server);
