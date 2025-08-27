#include "clippor-clipboard.h"
#include "clippor-database.h"
#include "clippor-entry.h"
#include "clippor-selection.h"
#include <glib-object.h>
#include <glib.h>
#include <stdint.h>

G_DEFINE_QUARK(CLIPPOR_CLIPBOARD_ERROR, clippor_clipboard_error)

// Holds important info when receiving data from a selection
typedef struct
{
    ClipporClipboard *cb;
    ClipporSelection *sel;
    ClipporEntry *entry;

    GPtrArray *mime_types;

    uint8_t buf[4096];
    GByteArray *data;
    uint index;
    uint cancel_sig_id;

    GCancellable *cancellable; // Same as the one in the clipboard
} ReceiveContext;

struct _ClipporClipboard
{
    GObject parent_instance;

    char *label;
    int64_t max_entries;

    ClipporDatabase *db;
    GPtrArray *selections;

    char *id; // Id of current entry

    ClipporEntry *entry; // Current entry that all selections are set to

    GCancellable *cancellable; // Used to cancel the current data receive
                               // operation

    GPtrArray *allowed_mime_types; // Array of GRegex objects.
    GHashTable *mime_type_groups; // Each key is a GRegex and its value is a ptr
                                  // array of mime types to expand to.
};

G_DEFINE_TYPE(ClipporClipboard, clippor_clipboard, G_TYPE_OBJECT)

typedef enum
{
    PROP_LABEL = 1,
    PROP_MAX_ENTRIES,
    PROP_ALLOWED_MIME_TYPES,
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
    case PROP_ALLOWED_MIME_TYPES:
        if (self->allowed_mime_types != NULL)
            g_ptr_array_unref(self->allowed_mime_types);
        self->allowed_mime_types = g_value_dup_boxed(value);
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
    case PROP_ALLOWED_MIME_TYPES:
        g_value_set_boxed(value, self->allowed_mime_types);
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
    g_clear_pointer(&self->allowed_mime_types, g_ptr_array_unref);

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
    obj_properties[PROP_ALLOWED_MIME_TYPES] = g_param_spec_boxed(
        "allowed-mime-types", "Allowed mime types",
        "Allowed mime types to store", G_TYPE_PTR_ARRAY, G_PARAM_READWRITE
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

/*
 * Return true if mime type is allowed to be stored in clipboard
 */
static gboolean
clippor_clipboard_mime_type_allowed(
    ClipporClipboard *self, const char *mime_type
)
{
    if (self->allowed_mime_types == NULL)
        return TRUE;
    for (uint i = 0; i < self->allowed_mime_types->len; i++)
    {
        GRegex *regex = self->allowed_mime_types->pdata[i];

        if (g_regex_match(regex, mime_type, G_REGEX_MATCH_DEFAULT, NULL))
            return TRUE;
    }
    return FALSE;
}

static void
selection_data_async_ready_callback(
    GInputStream *stream, GAsyncResult *result, ReceiveContext *ctx
)
{
    g_autoptr(GError) error = NULL;

    ssize_t r = g_input_stream_read_finish(stream, result, &error);

    if (r == -1)
    {
        // An error occured (ex. we got cancelled)
        if (error->code == G_IO_ERROR_CANCELLED)
            g_debug(
                "Data receive operation cancelled for clipboard '%s'",
                ctx->cb->label
            );
        else
            g_warning(
                "Data receive operation failed for clipboard '%s'",
                ctx->cb->label
            );
        goto fail;
    }
    else if (r == 0)
    {
        // EOF received
        const char *mime_type = ctx->mime_types->pdata[ctx->index];
        GBytes *bytes = g_byte_array_free_to_bytes(ctx->data);

        clippor_entry_add_mime_type(ctx->entry, mime_type, bytes);
        g_bytes_unref(bytes);

        if (ctx->index + 1 >= ctx->mime_types->len)
        {
finish:
            // We are finished
            if (ctx->cb->entry != NULL)
                g_object_unref(ctx->cb->entry);
            ctx->cb->entry = ctx->entry;

            selection_data_received(ctx->sel, ctx->cb);
            goto bail;
        }
        // Go onto next mime type (if not allowed go onto next)
        while (ctx->index++, ctx->index < ctx->mime_types->len)
            if (clippor_clipboard_mime_type_allowed(
                    ctx->cb, ctx->mime_types->pdata[ctx->index]
                ))
                break;

        // No more mime types in the list that are allowed, finish
        if (ctx->index >= ctx->mime_types->len)
            goto finish;

        GInputStream *new_stream = clippor_selection_get_data_stream(
            ctx->sel, ctx->mime_types->pdata[ctx->index], &error
        );

        if (new_stream == NULL)
        {
            g_assert(error != NULL);

            g_warning("Selection update failed: %s", error->message);

            g_object_unref(ctx->entry);
            goto bail;
        }

        ctx->data = g_byte_array_new();

        g_object_unref(stream);
        stream = new_stream;
    }
    else
        // Still more data to receive
        g_byte_array_append(ctx->data, ctx->buf, r);

    g_input_stream_read_async(
        stream, ctx->buf, 4096, G_PRIORITY_HIGH, ctx->cb->cancellable,
        (GAsyncReadyCallback)selection_data_async_ready_callback, ctx
    );

    return;

fail:
    g_object_unref(ctx->entry);
    g_byte_array_unref(ctx->data);
bail:
    // Only want to set it to NULL if it hasn't already been replaced
    if (ctx->cb->cancellable == ctx->cancellable)
        ctx->cb->cancellable = NULL;

    g_object_unref(stream);
    g_object_unref(ctx->sel);
    g_object_unref(ctx->cb);
    g_object_unref(ctx->cancellable);
    g_ptr_array_unref(ctx->mime_types);
    g_signal_handler_disconnect(ctx->sel, ctx->cancel_sig_id);
    g_free(ctx);
}

/*
 * Called when a selection wants us to stop receiving data for this operation.
 */
static void
cancel_receive_op_callback(
    ClipporSelection *sel G_GNUC_UNUSED, ReceiveContext *ctx
)
{
    g_cancellable_cancel(ctx->cancellable);
}

/*
 * Called when there is a new selection.
 */
static void
selection_update(ClipporSelection *sel, ClipporClipboard *cb)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) mime_types = clippor_selection_get_mime_types(sel);
    uint i = 0;

    if (mime_types->len <= 0)
        return;

    while (i < mime_types->len)
    {
        if (clippor_clipboard_mime_type_allowed(cb, mime_types->pdata[i]))
            break;
        i++;
    }

    GInputStream *stream =
        clippor_selection_get_data_stream(sel, mime_types->pdata[i], &error);

    if (stream == NULL)
    {
        g_warning("Selection update failed: %s", error->message);
        return;
    }

    // Check if we are in the middle of an existing operation, if so then cancel
    // it.
    if (cb->cancellable != NULL)
        g_cancellable_cancel(cb->cancellable);

    ReceiveContext *ctx = g_new(ReceiveContext, 1);

    ctx->cb = g_object_ref(cb);
    ctx->sel = g_object_ref(sel);
    ctx->entry = clippor_entry_new(cb);

    ctx->mime_types = g_ptr_array_ref(mime_types);

    ctx->data = g_byte_array_new();
    ctx->index = i;

    cb->cancellable = g_cancellable_new();
    ctx->cancellable = cb->cancellable;

    ctx->cancel_sig_id = g_signal_connect(
        sel, "cancel", G_CALLBACK(cancel_receive_op_callback), ctx
    );

    g_input_stream_read_async(
        stream, ctx->buf, 4096, G_PRIORITY_HIGH, cb->cancellable,
        (GAsyncReadyCallback)selection_data_async_ready_callback, ctx
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
