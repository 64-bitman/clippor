#include "clippor-clipboard.h"
#include "clippor-entry.h"
#include <glib-object.h>

G_DEFINE_ENUM_TYPE(
    ClipporSelectionType, clippor_selection_type,
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_NONE, "none"),
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_REGULAR, "regular"),
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_PRIMARY, "primary")
)

struct _ClipporClipboard
{
    GObject parent;

    gchar *label;

    // Each key is a label with the value being a weak pointer to the client
    // object.
    GHashTable *clients;

    guint64 max_entries;
    GQueue *entries; // Most recent being at the head of the queue
};

G_DEFINE_TYPE(ClipporClipboard, clippor_clipboard, G_TYPE_OBJECT)

typedef enum
{
    PROP_LABEL = 1,
    PROP_CLIENTS,
    PROP_MAX_ENTRIES,
    N_PROPERTIES
} ClipporClipboardProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {
    NULL,
};

static void callback_client_unref(gpointer weak_ref);

static void
clippor_clipboard_set_property(
    GObject *object, guint property_id, const GValue *value, GParamSpec *pspec
)
{
    ClipporClipboard *self = CLIPPOR_CLIPBOARD(object);

    switch ((ClipporClipboardProperty)property_id)
    {
    case PROP_LABEL:
        g_free(self->label);
        self->label = g_value_dup_string(value);
        break;
    case PROP_MAX_ENTRIES:
        self->max_entries = g_value_get_uint64(value);
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

    switch ((ClipporClipboardProperty)property_id)
    {
    case PROP_LABEL:
        g_value_set_string(value, self->label);
        break;
    case PROP_MAX_ENTRIES:
        g_value_set_uint64(value, self->max_entries);
        break;
    case PROP_CLIENTS:
        g_value_set_boxed(value, self->clients);
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

    g_hash_table_remove_all(self->clients);
    g_queue_clear_full(self->entries, g_object_unref);

    G_OBJECT_CLASS(clippor_clipboard_parent_class)->dispose(object);
}

static void
clippor_clipboard_finalize(GObject *object)
{
    ClipporClipboard *self = CLIPPOR_CLIPBOARD(object);

    g_free(self->label);

    g_hash_table_unref(self->clients);
    g_queue_free(self->entries);

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
        "label", "Label", "Label of clipboard", "Untitled Clipboard",
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );
    obj_properties[PROP_CLIENTS] = g_param_spec_boxed(
        "selections", "Selections", "Selections attached to this clipboard",
        G_TYPE_HASH_TABLE, G_PARAM_READABLE
    );
    obj_properties[PROP_MAX_ENTRIES] = g_param_spec_uint64(
        "max-entries", "Max entries",
        "Maximum amount of entries stored in memory", 1, G_MAXUINT64, 10,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
    );
}

static void
clippor_clipboard_init(ClipporClipboard *self)
{
    self->clients = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, g_free, callback_client_unref
    );
    self->entries = g_queue_new();
}

ClipporClipboard *
clippor_clipboard_new(const gchar *label)
{
    g_return_val_if_fail(label != NULL, NULL);

    ClipporClipboard *cb =
        g_object_new(CLIPPOR_TYPE_CLIPBOARD, "label", label, NULL);

    return cb;
}

/*
 * Free the weak reference object for client
 */
static void
callback_client_unref(gpointer weak_ref)
{
    g_weak_ref_clear(weak_ref);
    g_free(weak_ref);
}

/*
 *
 */
static guint64
get_new_entry_index(ClipporClipboard *self)
{
    g_return_val_if_fail(CLIPPOR_IS_CLIPBOARD(self), 0);

    ClipporEntry *entry = g_queue_peek_head(self->entries);

    if (entry == NULL)
        return 0;
    else
        return clippor_entry_get_index(entry) + 1;
}

void
clippor_clipboard_add_entry(ClipporClipboard *self, ClipporEntry *entry)
{
    g_return_if_fail(CLIPPOR_IS_CLIPBOARD(self));
    g_return_if_fail(CLIPPOR_IS_ENTRY(entry));

    // Shove out excess old entries until there is one spot available
    while (self->entries->length >= self->max_entries)
        g_object_unref(g_queue_pop_tail(self->entries));

    g_queue_push_head(self->entries, entry);
}

static void
callback_send_data_hash_table(
    GObject *client G_GNUC_UNUSED, GHashTable *mime_types, gpointer data
)
{
    GHashTable **table = data;

    *table = g_hash_table_ref(mime_types);
}

/*
 * Called when there is a new selection from a client. If so then update the
 * other clients and add a new entry to the queue.
 */
static void
callback_client_selection(
    GObject *client, ClipporSelectionType selection, gpointer data
)
{
    ClipporClipboard *cb = data;

    ClipporEntry *entry = clippor_entry_new(get_new_entry_index(cb), client);
    GHashTable *mime_types = NULL;

    const gchar *signal_name;
    const gchar *property_name;

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
    {
        signal_name = "send-data::regular";
        property_name = "regular-entry";
    }
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
    {
        signal_name = "send-data::primary";
        property_name = "primary-entry";
    }
    else
        return;

    // Get mime types and data.
    gulong handler = g_signal_connect(
        client, signal_name, G_CALLBACK(callback_send_data_hash_table),
        &mime_types
    );
    g_object_set(client, "send-data", selection, NULL);

    g_signal_handler_disconnect(client, handler);

    if (mime_types == NULL)
    {
        // An error occured
        g_info(
            "Could not update selection from new entry for clipboard '%s'",
            cb->label
        );
        g_object_unref(entry);
        return;
    }

    clippor_entry_set_mime_types(entry, mime_types, TRUE);

    clippor_clipboard_add_entry(cb, entry);

    // Update clients (includes the source client too)
    GHashTableIter iter;
    GWeakRef *weak_ref;

    g_hash_table_iter_init(&iter, cb->clients);

    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&weak_ref))
    {
        GObject *client = g_weak_ref_get(weak_ref);

        if (client == NULL)
            // Client was finalized, remove it
            g_hash_table_iter_remove(&iter);

        // Set the respective entry to the current one
        g_object_set(client, property_name, entry, NULL);
    }
}

void
clippor_clipboard_add_client(
    ClipporClipboard *self, const char *label, GObject *client,
    ClipporSelectionType selection
)
{
    g_return_if_fail(CLIPPOR_IS_CLIPBOARD(self));
    g_return_if_fail(G_IS_OBJECT(client));
    g_return_if_fail(label != NULL);

    GWeakRef *ref = g_new(GWeakRef, 1);

    // If g_object_weak_ref is used, every time the callback is called, we'd
    // have to loop through all the entries, and destroy the object that
    // matches. Additionally, we can't use the normal destroy function for the
    // hash table, we must loop through all the entries.
    // 
    // Honestly seems more complicated than just removing the entry when we loop
    // through the haash table on a new selection.
    g_weak_ref_init(ref, client);

    g_hash_table_insert(self->clients, g_strdup(label), ref);

    // Update client selection

    gchar *entry_property;
    gchar *signal_name;

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
    {
        entry_property = "regular-entry";
        signal_name = "selection::regular";
    }
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
    {
        entry_property = "primary-entry";
        signal_name = "selection::primary";
    }
    else
        return;

    ClipporEntry *entry = g_queue_peek_head(self->entries);

    if (entry != NULL)
        g_object_set(client, entry_property, entry, NULL);

    // Start listening for new selections
    g_signal_connect(
        client, signal_name, G_CALLBACK(callback_client_selection), self
    );
}
