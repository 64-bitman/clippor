#include "wayland-selection.h"
#include "wayland-connection.h"
#include <glib-object.h>
#include <glib-unix.h>
#include <glib.h>

struct _WaylandSelection
{
    ClipporSelection parent_instance;

    WaylandSeat *seat; // Don't create new reference, it will outlive us anyways

    GHashTable *mime_types;

    WaylandDataOffer *offer;
    WaylandDataSource *source;

    gboolean active;
};

G_DEFINE_TYPE(WaylandSelection, wayland_selection, CLIPPOR_TYPE_SELECTION)

static void
wayland_selection_dispose(GObject *object)
{
    G_OBJECT_CLASS(wayland_selection_parent_class)->dispose(object);
}

static void
wayland_selection_finalize(GObject *object)
{
    G_OBJECT_CLASS(wayland_selection_parent_class)->finalize(object);
}

static const GPtrArray *class_method_get_mime_types(ClipporSelection *self);

static void
wayland_selection_class_init(WaylandSelectionClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);
    ClipporSelectionClass *sel_class = CLIPPOR_SELECTION_CLASS(class);

    gobject_class->dispose = wayland_selection_dispose;
    gobject_class->finalize = wayland_selection_finalize;

    sel_class->get_mime_types = class_method_get_mime_types;
}

static void
wayland_selection_init(WaylandSelection *self G_GNUC_UNUSED)
{
}

WaylandSelection *
wayland_selection_new(WaylandSeat *seat, ClipporSelectionType type)
{
    g_assert(type != CLIPPOR_SELECTION_TYPE_NONE);

    WaylandSelection *wsel =
        g_object_new(WAYLAND_TYPE_SELECTION, "type", type, NULL);

    wsel->active = TRUE;
    wsel->seat = seat;

    return wsel;
}

/*
 * Make selection object inert. This means it will not emit any signals and any
 * calls on it will be ignored or return an error value. Cannot be undone
 */
void
wayland_selection_make_inert(WaylandSelection *self)
{
    g_assert(WAYLAND_IS_SELECTION(self));

    if (!self->active)
        return;

    g_clear_pointer(&self->offer, wayland_data_offer_destroy);
    g_clear_pointer(&self->source, wayland_data_source_destroy);

    g_clear_pointer(&self->mime_types, g_hash_table_unref);
}

void
wayland_selection_unref_and_inert(WaylandSelection *self)
{
    g_assert(WAYLAND_IS_SELECTION(self));

    wayland_selection_make_inert(self);
    g_object_unref(self);
}

/*
 * Set the current offer used by selection and destroy the previous if any, then
 * emit the "update" signal. The offer should be valid. The function will take a
 * new reference to the ptr array. If a NULL offer is passed then the selection
 * is assumed to be cleared.
 */
void
wayland_selection_set_offer(WaylandSelection *self, WaylandDataOffer *offer)
{
    g_assert(WAYLAND_IS_SELECTION(self));
    g_assert(offer == NULL || wayland_data_offer_is_valid(offer));

    // Destroy previous offer and resources associated with it
    wayland_data_offer_destroy(self->offer);

    self->offer = offer;

    g_signal_emit_by_name(CLIPPOR_SELECTION(self), "update");
}

static const GPtrArray *
class_method_get_mime_types(ClipporSelection *self)
{
    g_assert(CLIPPOR_IS_SELECTION(self));

    WaylandSelection *seat = WAYLAND_SELECTION(self);

    return seat->offer == NULL ? NULL
                               : wayland_data_offer_get_mime_types(seat->offer);
}

static GBytes *
class_method_start_get_data(
    ClipporSelection *self, const char *mime_type, GError **error
)
{
    GUnixPipe pipe;


}
