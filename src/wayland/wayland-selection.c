#include "wayland-selection.h"
#include "wayland-connection.h"
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib-object.h>
#include <glib-unix.h>
#include <glib.h>

struct _WaylandSelection
{
    ClipporSelection parent_instance;

    WaylandSeat *seat; // Don't create new reference, it will outlive us anyways

    WaylandDataOffer *offer;

    WaylandDataSource *source;

    gboolean active;
};

G_DEFINE_TYPE(WaylandSelection, wayland_selection, CLIPPOR_TYPE_SELECTION)

static void
wayland_selection_dispose(GObject *object)
{
    WaylandSelection *self = WAYLAND_SELECTION(object);

    wayland_selection_make_inert(self);

    G_OBJECT_CLASS(wayland_selection_parent_class)->dispose(object);
}

static void
wayland_selection_finalize(GObject *object)
{
    G_OBJECT_CLASS(wayland_selection_parent_class)->finalize(object);
}

// Class method handlers
static GPtrArray *
clippor_selection_handler_get_mime_types(ClipporSelection *self);
static GInputStream *clippor_selection_handler_get_data_stream(
    ClipporSelection *self, const char *mime_type, GError **error
);
static gboolean clippor_selection_handler_update(
    ClipporSelection *self, ClipporEntry *entry, gboolean is_source,
    GError **error
);
static gboolean clippor_selection_handler_is_owned(ClipporSelection *self);
static gboolean clippor_selection_handler_is_inert(ClipporSelection *self);

static void
wayland_selection_class_init(WaylandSelectionClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);
    ClipporSelectionClass *sel_class = CLIPPOR_SELECTION_CLASS(class);

    gobject_class->dispose = wayland_selection_dispose;
    gobject_class->finalize = wayland_selection_finalize;

    sel_class->get_mime_types = clippor_selection_handler_get_mime_types;
    sel_class->get_data_stream = clippor_selection_handler_get_data_stream;
    sel_class->update = clippor_selection_handler_update;
    sel_class->is_owned = clippor_selection_handler_is_owned;
    sel_class->is_inert = clippor_selection_handler_is_inert;
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
    self->seat = NULL;
    self->active = FALSE;
}

void
wayland_selection_unref_and_inert(WaylandSelection *self)
{
    g_assert(WAYLAND_IS_SELECTION(self));

    wayland_selection_make_inert(self);
    g_object_unref(self);
}

gboolean
wayland_selection_is_active(WaylandSelection *self)
{
    g_assert(WAYLAND_IS_SELECTION(self));

    return self->active;
}

static void
send_data_async_callback(GObject *object, GAsyncResult *result, void *user_data)
{
    GOutputStream *stream = G_OUTPUT_STREAM(object);
    GBytes *bytes = user_data;

    GError *error = NULL;
    ssize_t w = g_output_stream_write_bytes_finish(stream, result, &error);

    if (w == -1)
    {
        // An error occured
        g_warning("Failed sending data: %s", error->message);
        g_error_free(error);
    }
    else if (w > 0)
    {
        size_t bytes_size = g_bytes_get_size(bytes);

        // Check if we stil have more to write
        if ((size_t)w < bytes_size)
        {
            GBytes *new_bytes =
                g_bytes_new_from_bytes(bytes, w, bytes_size - w);

            g_bytes_unref(bytes);

            g_output_stream_write_bytes_async(
                stream, bytes, G_PRIORITY_HIGH, NULL, send_data_async_callback,
                new_bytes
            );
            return;
        }
    }

    // Done writing everything or an error occured
    g_bytes_unref(bytes);
    g_object_unref(stream);
}

static void
data_source_listener_event_send(
    void *data, WaylandDataSource *source G_GNUC_UNUSED, const char *mime_type,
    int fd
)
{
    WaylandSelection *wsel = data;
    ClipporEntry *entry = clippor_selection_get_entry(CLIPPOR_SELECTION(wsel));
    GBytes *bytes = clippor_entry_get_data(entry, mime_type);

    // Shouldn't happen...
    if (bytes == NULL)
    {
        close(fd);
        return;
    }

    // Send data asynchronously
    GOutputStream *stream = g_unix_output_stream_new(fd, TRUE);

    g_output_stream_write_bytes_async(
        stream, bytes, G_PRIORITY_HIGH, NULL, send_data_async_callback,
        g_bytes_ref(bytes)
    );
}

/*
 * Called when there is a new source client. Let them be the source until the
 * selection is cleared or becomes empty, such as when the source client exits.
 */
static void
data_source_listener_event_cancelled(void *data, WaylandDataSource *source)
{
    WaylandSelection *wsel = data;

    wayland_data_source_destroy(source);

    // Only set it to NULL if it is the same, because if we set the selection,
    // we will receive the cancelled event, and we don't want to discard the
    // source we just set by setting it to NULL.
    if (wsel->source == source)
        wsel->source = NULL;
}

static const WaylandDataSourceListener data_source_listener = {
    .send = data_source_listener_event_send,
    .cancelled = data_source_listener_event_cancelled
};

/*
 * Become the source client for the selection, using the currently set entry. If
 * the entry is NULL, clear the selection.
 */
static void
wayland_selection_own(WaylandSelection *self)
{
    ClipporEntry *entry = clippor_selection_get_entry(CLIPPOR_SELECTION(self));

    if (entry != NULL)
    {
        WaylandDataDeviceManager *manager =
            wayland_seat_get_data_device_manager(self->seat);

        self->source = wayland_data_device_manager_create_data_source(manager);

        wayland_data_source_add_listener(
            self->source, &data_source_listener, self
        );

        // Export mime types
        GHashTableIter iter;
        const char *mime_type;

        g_hash_table_iter_init(&iter, clippor_entry_get_mime_types(entry));

        while (g_hash_table_iter_next(&iter, (void **)&mime_type, NULL))
            wayland_data_source_offer(self->source, mime_type);
    }
    else
        self->source = NULL;

    WaylandDataDevice *device = wayland_seat_get_data_device(self->seat);
    ClipporSelectionType type;

    g_object_get(self, "type", &type, NULL);

    wayland_data_device_set_selection(device, self->source, type);

    // Let the context process/flush events and buffer. This avoids situations
    // where we start trying to receive data from an offer that is replaced
    // right after with the one from the source we set here.
}

/*
 * Set the current offer used by selection and destroy the previous if any, then
 * emit the "update" signal. The offer should be valid or NULL (if selection is
 * cleared).
 *
 * The function will take a new reference to the ptr array.
 *
 * If a NULL offer is passed then the selection is assumed to be cleared.
 */
void
wayland_selection_new_offer(WaylandSelection *self, WaylandDataOffer *offer)
{
    g_assert(WAYLAND_IS_SELECTION(self));
    g_assert(offer == NULL || wayland_data_offer_is_valid(offer));

    // Destroy previous offer and resources associated with it
    wayland_data_offer_destroy(self->offer);

    if (self->source != NULL)
    {
        // We are the source client, ignore and destroy the offer
        wayland_data_offer_destroy(offer);
        self->offer = NULL;
        return;
    }
    self->offer = offer;

    // If offer is NULL, set the selection instead of emitting signal, only if
    // the entry is not NULL, since we would just be clearing it again
    // redundantly.
    if (offer == NULL)
    {
        ClipporEntry *entry =
            clippor_selection_get_entry(CLIPPOR_SELECTION(self));

        if (entry == NULL)
            return;

        wayland_selection_own(self);
    }
    else
        g_signal_emit_by_name(CLIPPOR_SELECTION(self), "update");
}

static GPtrArray *
clippor_selection_handler_get_mime_types(ClipporSelection *self)
{
    g_assert(WAYLAND_IS_SELECTION(self));

    WaylandSelection *wsel = WAYLAND_SELECTION(self);

    if (!wsel->active)
        return NULL;

    return wsel->offer == NULL
               ? NULL
               : g_ptr_array_ref(
                     wayland_data_offer_get_mime_types(wsel->offer)
                 );
}

static GInputStream *
clippor_selection_handler_get_data_stream(
    ClipporSelection *self, const char *mime_type, GError **error
)
{
    g_assert(WAYLAND_IS_SELECTION(self));
    g_assert(mime_type != NULL);
    g_assert(error == NULL || *error == NULL);

    WaylandSelection *wsel = WAYLAND_SELECTION(self);

    if (!wsel->active)
    {
        g_set_error(
            error, CLIPPOR_SELECTION_ERROR, CLIPPOR_SELECTION_ERROR_INERT,
            "Failed creating input stream: Selection is inert"
        );
        return NULL;
    }

    if (wsel->offer == NULL)
    {
        g_set_error(
            error, CLIPPOR_SELECTION_ERROR, CLIPPOR_SELECTION_ERROR_CLEARED,
            "Failed creating input stream: Selection is cleared"
        );
        return NULL;
    }

    // Create pipe
    int fds[2];

    if (!g_unix_open_pipe(fds, O_CLOEXEC, error))
    {
        g_prefix_error_literal(error, "Failed opening pipe: ");
        return NULL;
    }

    wayland_data_offer_receive(wsel->offer, mime_type, fds[1]);

    // Close our write-end because we don't need it
    close(fds[1]);

    if (!wayland_connection_flush(
            wayland_seat_get_connection(wsel->seat), error
        ))
    {
        close(fds[0]);
        return FALSE;
    }

    GInputStream *stream = g_unix_input_stream_new(fds[0], TRUE);

    return stream;
}

// TODO: refactor error enums into more general topics.

/*
 * If an error occurs, then the selection will use the passed entry instead of
 * the previous one.
 */
static gboolean
clippor_selection_handler_update(
    ClipporSelection *self, ClipporEntry *entry, gboolean is_source,
    GError **error
)
{
    g_assert(WAYLAND_IS_SELECTION(self));
    g_assert(entry == NULL || CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    WaylandSelection *wsel = WAYLAND_SELECTION(self);

    if (!wsel->active)
    {
        g_set_error(
            error, CLIPPOR_SELECTION_ERROR, CLIPPOR_SELECTION_ERROR_INERT,
            "Failed updating Wayland selection: Selection is inert"
        );
        return FALSE;
    }

    g_object_set(wsel, "entry", entry, NULL);

    // Obviously don't want to set the selection again right after we set it...
    if (!is_source)
        wayland_selection_own(wsel);

    return TRUE;
}

static gboolean
clippor_selection_handler_is_owned(ClipporSelection *self)
{
    g_assert(WAYLAND_IS_SELECTION(self));

    WaylandSelection *wsel = WAYLAND_SELECTION(self);

    return wsel->source != NULL && wsel->active;
}

static gboolean
clippor_selection_handler_is_inert(ClipporSelection *self)
{
    g_assert(WAYLAND_IS_SELECTION(self));

    WaylandSelection *wsel = WAYLAND_SELECTION(self);

    return !wsel->active;
}
