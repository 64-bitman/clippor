#pragma once

#include <glib.h>

typedef struct
{
    GPid pid;
    gchar *display;
    gchar *restore_display; // Previous display if we set $WAYLAND_DISPLAY to
                            // display.
} WaylandCompositor;

WaylandCompositor *wayland_compositor_new(gboolean set_env);

void wayland_compositor_destroy(WaylandCompositor *self);

void wayland_compositor_set_env(WaylandCompositor *self);
void wayland_compositor_restore_env(WaylandCompositor *self);

void wl_copy(gboolean primary, gchar *format, ...);

gchar *wl_paste(gboolean primary, gboolean newline, gchar *mime_type);

void set_sigabrt_handler(struct sigaction *sa);
