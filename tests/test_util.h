#pragma once

#include "clippor-client.h"
#include <glib-object.h>
#include <glib.h>

typedef struct
{
    GPid pid;
    gchar *display;
    gchar *restore_display; // Previous display if we set $WAYLAND_DISPLAY to
                            // display.
} WaylandCompositor;

typedef struct
{
    GPtrArray *mime_types;
    GBytes *data;
    GWeakRef entry;
} TesterClientSelection;

#define TESTER_TYPE_CLIENT (tester_client_get_type())

G_DECLARE_FINAL_TYPE(TesterClient, tester_client, TESTER, CLIENT, ClipporClient)

TesterClient *tester_client_new(void);

TesterClientSelection *
tester_client_get_selection(TesterClient *self, ClipporSelectionType selection);

WaylandCompositor *wayland_compositor_new(gboolean set_env);

void wayland_compositor_destroy(WaylandCompositor *self);

void wayland_compositor_set_env(WaylandCompositor *self);
void wayland_compositor_restore_env(WaylandCompositor *self);

void wl_copy(gboolean primary, gchar *format, ...);

gchar *wl_paste(gboolean primary, gboolean newline, gchar *mime_type);

void set_sigabrt_handler(struct sigaction *sa);
