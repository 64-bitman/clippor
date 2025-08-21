#include "clippor-clipboard.h"
#include "clippor-database.h"
#include "clippor-entry.h"
#include "clippor-selection.h"
#include <glib-object.h>
#include <glib.h>
#include <stdint.h>

G_DEFINE_QUARK(CLIPPOR_CLIPBOARD_ERROR, clippor_clipboard_error)

struct _ClipporClipboard
{
    GObject parent_instance;

    char *label;
    int64_t max_entries;

    ClipporDatabase *db;
    GPtrArray *selections;

    char *id; // Id of current entry

    // Context used when receiving data
    struct
    {
        ClipporSelection *sel;

        uint8_t buf[4096];
        GByteArray *arr;
        GCancellable *cancel;

        GPtrArray *mime_types;
        uint index;
    } receive_ctx;
    gboolean receiving;

    ClipporEntry *entry; // Current entry that all selections are set to

    GCancellable *cancellable; // Cancellable used for all input streams
};

G_DEFINE_TYPE(ClipporClipboard, clippor_clipboard, G_TYPE_OBJECT)

typedef enum
{
    PROP_LABEL = 1,
    PROP_MAX_ENTRIES,
    N_PROPERTIES
} ClipporClipboardProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {NULL};

static void
clippor_clipboard_set_property(
    GObject *object, guint property_id, const GValue *value, GParamSpec *pspec
)
{
    ClipporClipboard *self = CLIPPOR_CLIPBOARD(object);

    switch (property_id)
    {
    case PROP_LABEL:
        g_free(self->label);
        self->label = g_value_dup_string(value);
        break;
    case PROP_MAX_ENTRIES:
        // TODO: also trim entries in database as well
        self->max_entries = g_value_get_int64(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
clippor_clipboard_get_property(
    GObject *object, guint property_id, GValue *value, GParamSpec *pspec
)
{
    ClipporClipboard *self = CLIPPOR_CLIPBOARD(object);

    switch (property_id)
    {
    case PROP_LABEL:
        g_value_set_string(value, self->label);
        break;
    case PROP_MAX_ENTRIES:
        g_value_set_int64(value, self->max_entries);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
clippor_clipboard_dispose(GObject *object)
{
    ClipporClipboard *self = CLIPPOR_CLIPBOARD(object);

    g_clear_object(&self->db);
    g_clear_pointer(&self->selections, g_ptr_array_unref);
    g_clear_object(&self->entry);

    G_OBJECT_CLASS(clippor_clipboard_parent_class)->dispose(object);
}

static void
clippor_clipboard_finalize(GObject *object)
{
    ClipporClipboard *self = CLIPPOR_CLIPBOARD(object);

    g_free(self->label);

    G_OBJECT_CLASS(clippor_clipboard_parent_class)->finalize(object);
}

static void
clippor_clipboard_class_init(ClipporClipboardClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->set_property = clippor_clipboard_set_property;
    gobject_class->get_property = clippor_clipboard_get_property;

    gobject_class->dispose = clippor_clipboard_dispose;
    gobject_class->finalize = clippor_clipboard_finalize;

    obj_properties[PROP_LABEL] = g_param_spec_string(
        "label", "Label", "Label of clipboard", "",
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );
    obj_properties[PROP_MAX_ENTRIES] = g_param_spec_int64(
        "max-entries", "Max entries", "Maximum number of entries in history", 1,
        G_MAXINT64, 100, G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
    );
}

static void
clippor_clipboard_init(ClipporClipboard *self)
{
    self->selections = g_ptr_array_new_with_free_func(g_object_unref);
}

/*
 * If db is NULL then the clipboard will run without a database and will only
 * save the current selection. This makes the clipboard only persist selections,
 * like wl_clip_persist.
 */
ClipporClipboard *
clippor_clipboard_new(const char *label)
{
    g_assert(label != NULL);

    ClipporClipboard *cb =
        g_object_new(CLIPPOR_TYPE_CLIPBOARD, "label", label, NULL);

    return cb;
}

/*
 * Update all selections connected to clipboard with the currently set entry.
 * If "sel" is not NULL, it should be the object that caused the clipboard to
 * update all the selections.
 */
static void
clippor_clipboard_update_selections(
    ClipporClipboard *self, ClipporSelection *sel
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));
    g_assert(sel == NULL || CLIPPOR_IS_SELECTION(sel));

    // Update selections
    for (uint i = 0; i < self->selections->len; i++)
    {
        ClipporSelection *s = self->selections->pdata[i];
        GError *error = NULL;

        if (clippor_selection_is_inert(s))
        {
            // Remove it
            g_ptr_array_remove(self->selections, s);
            continue;
        }

        if (!clippor_selection_update(s, self->entry, s == sel, &error))
        {
            g_assert(error != NULL);
            g_warning("%s", error->message);
            g_error_free(error);
            continue;
        }
    }
}

gboolean
clippor_clipboard_set_database(
    ClipporClipboard *self, ClipporDatabase *db, GError **error
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));
    g_assert(CLIPPOR_IS_DATABASE(db));
    g_assert(error == NULL || *error == NULL);

    if (self->db != NULL)
        g_object_unref(self->db);
    self->db = g_object_ref(db);

    self->entry =
        clippor_database_deserialize_entry_at_index(db, self->label, 0, error);

    // Ignore if database is just empty
    if (self->entry == NULL &&
        (*error)->code != CLIPPOR_DATABASE_ERROR_ROW_NOT_EXIST)
    {
        g_prefix_error(error, "Failed loading clipboard '%s': ", self->label);
        return FALSE;
    }
    g_clear_error(error);

    if (self->entry != NULL)
        clippor_clipboard_update_selections(self, NULL);

    return TRUE;
}

/*
 * Called when we received all data for every mime type for the new selection.
 */
static void
selection_data_received(ClipporSelection *sel, ClipporClipboard *cb)
{
    g_assert(CLIPPOR_IS_SELECTION(sel));
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));

    // Update database if we are attached to one
    if (cb->db != NULL)
    {
        GError *error = NULL;

        if (!clippor_database_serialize_entry(cb->db, cb->entry, &error))
        {
            g_assert(error != NULL);
            g_warning("Failed serializing entry: %s", error->message);
            g_error_free(error);
        }

        if (!clippor_database_trim_entries(
                cb->db, cb->label, cb->max_entries, &error
            ))
        {
            g_assert(error != NULL);
            g_warning("Failed trimming database: %s", error->message);
            g_error_free(error);
        }
    }

    clippor_clipboard_update_selections(cb, sel);
}

static void
selection_data_async_callback(
    GObject *object, GAsyncResult *result, void *user_data
)
{
    GInputStream *stream = G_INPUT_STREAM(object);
    ClipporClipboard *cb = user_data;
    GError *error = NULL;

    ssize_t r = g_input_stream_read_finish(stream, result, &error);

    const char *mime_type =
        cb->receive_ctx.mime_types->pdata[cb->receive_ctx.index];

    if (r == -1)
    {
        // An error occured or operation was cancelled
        if (error->code != G_IO_ERROR_CANCELLED)
            g_debug(
                "Input stream did not finish properly: %s", error->message
            );
        goto fail;
    }
    else if (r == 0)
    {
        g_clear_object(&stream);
        // EOF recieved, go onto next mime type if possible
        cb->receive_ctx.index++;

        GBytes *bytes = g_byte_array_free_to_bytes(cb->receive_ctx.arr);

        cb->receive_ctx.arr = NULL;

        clippor_entry_add_mime_type(cb->entry, mime_type, bytes);
        g_bytes_unref(bytes);

        if (cb->receive_ctx.index == cb->receive_ctx.mime_types->len)
        {
            // Received all mime types, we are done
            cb->receiving = FALSE;

            selection_data_received(cb->receive_ctx.sel, cb);

            g_ptr_array_unref(cb->receive_ctx.mime_types);
            g_object_unref(cb->receive_ctx.cancel);
            g_object_unref(cb->receive_ctx.sel);
            g_object_unref(cb);

            return;
        }

        // Reinitialize values to use new mime type and get input stream for it
        const char *new_mime_type =
            cb->receive_ctx.mime_types->pdata[cb->receive_ctx.index];

        stream = clippor_selection_get_data(
            cb->receive_ctx.sel, new_mime_type, &error
        );

        if (stream == NULL)
        {
            g_debug("Failed creating input stream: %s", error->message);
            goto fail;
        }
        else
            cb->receive_ctx.arr = g_byte_array_new();
    }
    else
        // Still more data to receive
        g_byte_array_append(cb->receive_ctx.arr, cb->receive_ctx.buf, r);

    g_input_stream_read_async(
        stream, cb->receive_ctx.buf, 4096, G_PRIORITY_HIGH,
        cb->receive_ctx.cancel, selection_data_async_callback, cb
    );

    return;
fail:
    cb->receiving = FALSE;
    g_error_free(error);
    g_clear_object(&stream);

    // Clean out any progress we made
    g_clear_object(&cb->entry);

    if (cb->receive_ctx.arr != NULL)
        g_byte_array_unref(cb->receive_ctx.arr);
    g_ptr_array_unref(cb->receive_ctx.mime_types);

    g_object_unref(cb->receive_ctx.cancel);
    g_object_unref(cb->receive_ctx.sel);
    g_object_unref(cb);
}

/*
 * Called when there is a new selection.
 */
static void
selection_update(ClipporSelection *sel, ClipporClipboard *cb)
{
    GError *error = NULL;

    // Check if we are already receiving data, if so cancel it
    if (cb->receiving)
    {
        g_cancellable_cancel(cb->receive_ctx.cancel);

        // Cancellable in GIO is async, so iterate the context
        while (cb->receiving)
            g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
    }

    g_autoptr(GPtrArray) mime_types = clippor_selection_get_mime_types(sel);
    GInputStream *stream =
        clippor_selection_get_data(sel, mime_types->pdata[0], &error);

    if (stream == NULL)
    {
        g_assert(error != NULL);
        g_warning("Selection update failed: %s", error->message);
        g_error_free(error);
        return;
    }

    // Create a new entry to store the mime types.
    if (cb->entry != NULL)
        g_object_unref(cb->entry);

    cb->entry = clippor_entry_new(cb);

    cb->receive_ctx.sel = g_object_ref(sel);
    cb->receive_ctx.cancel = g_cancellable_new();
    cb->receive_ctx.mime_types = g_ptr_array_ref(mime_types);
    cb->receive_ctx.index = 0;
    cb->receive_ctx.arr = g_byte_array_new();
    cb->receiving = TRUE;

    // Start receiving data async
    g_input_stream_read_async(
        stream, cb->receive_ctx.buf, 4096, G_PRIORITY_HIGH,
        cb->receive_ctx.cancel, selection_data_async_callback, g_object_ref(cb)
    );
}

void
clippor_clipboard_add_selection(ClipporClipboard *self, ClipporSelection *sel)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));
    g_assert(CLIPPOR_IS_SELECTION(sel));

    if (g_ptr_array_find(self->selections, sel, NULL))
        return;

    g_ptr_array_add(self->selections, g_object_ref(sel));

    GError *error = NULL;

    // Set selection to current entry for clipboard
    if (!clippor_selection_update(sel, self->entry, FALSE, &error))
    {
        g_warning("%s", error->message);
        g_error_free(error);
    }

    // Listen for new updates
    g_signal_connect_object(
        sel, "update", G_CALLBACK(selection_update), self, G_CONNECT_DEFAULT
    );
}

/*
 * Same as clippor_clipboard_add_selection but doesn't update selection
 */
void
clippor_clipboard_connect_selection(
    ClipporClipboard *self, ClipporSelection *sel
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));
    g_assert(CLIPPOR_IS_SELECTION(sel));

    if (g_ptr_array_find(self->selections, sel, NULL))
        return;

    g_ptr_array_add(self->selections, g_object_ref(sel));

    g_signal_connect_object(
        sel, "update", G_CALLBACK(selection_update), self, G_CONNECT_DEFAULT
    );
}

const char *
clippor_clipboard_get_label(ClipporClipboard *self)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));

    return self->label;
}

/*
 * Return current entry. Entry object is owned by the clipboard
 */
ClipporEntry *
clippor_clipboard_get_entry(ClipporClipboard *self)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));

    return self->entry;
}
