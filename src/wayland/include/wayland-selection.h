#pragma once

#include "clippor-selection.h"
#include "wayland-connection.h"
#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(
    WaylandSelection, wayland_selection, WAYLAND, SELECTION, ClipporSelection
)
#define WAYLAND_TYPE_SELECTION (wayland_selection_get_type())

typedef enum
{
    WAYLAND_SELECTION_ERROR_CLEARED,
    WAYLAND_SELECTION_ERROR_INERT
} WaylandSelectionError;

#define WAYLAND_SELECTION_ERROR (wayland_selection_error_quark())
GQuark wayland_selection_error_quark(void);

typedef struct _WaylandSeat WaylandSeat;

WaylandSelection *
wayland_selection_new(WaylandSeat *seat, ClipporSelectionType type);

void wayland_selection_make_inert(WaylandSelection *self);
void wayland_selection_unref_and_inert(WaylandSelection *self);

gboolean wayland_selection_is_active(WaylandSelection *self);

void
wayland_selection_new_offer(WaylandSelection *self, WaylandDataOffer *offer);
