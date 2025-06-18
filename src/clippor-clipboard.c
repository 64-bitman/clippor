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

    // Each key is the label and value is the client object. Do not create new
    // references for client objects
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

typedef enum
{
    SIGNAL_CLIENT_REMOVED,
    N_SIGNALS
} WaylandSeatSignal;

static guint obj_signals[N_SIGNALS] = {0};

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

    obj_signals[SIGNAL_CLIENT_REMOVED] = g_signal_new(
        "client-removed", G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_FIRST | G_SIGNAL_NO_HOOKS | G_SIGNAL_NO_RECURSE, 0, NULL,
        NULL, NULL, G_TYPE_NONE, 1, G_TYPE_OBJECT
    );
}

static void
clippor_clipboard_init(ClipporClipboard *self)
{
    self->clients = g_hash_table_new_full(
        g_direct_hash, g_direct_equal, g_free, g_object_unref
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
 * Resizes the history queue accordingly to max_entries.
 */
void
clippor_clipboard_update_history(ClipporClipboard *self)
{
    g_return_if_fail(CLIPPOR_IS_CLIPBOARD(self));

    while (self->entries->length >= self->max_entries)
        g_queue_pop_tail(self->entries);
}

/*
 * Creates a new entry given the mime types and adds it the history queue
 */
void
clippor_clipboard_new_entry(
    ClipporClipboard *self, GHashTable *mime_types, GObject *source
)
{
    g_return_if_fail(CLIPPOR_IS_CLIPBOARD(self));
    g_return_if_fail(mime_types != NULL);

    clippor_clipboard_update_history(self);

    ClipporEntry *cur = g_queue_peek_head(self->entries);
    ClipporEntry *entry;

    GHashTableIter iter;
    char *mime_type;
    GBytes *data;

    g_hash_table_iter_init(&iter, mime_types);

    if (cur == NULL)
        entry = clippor_entry_new(0, source);
    else
        entry = clippor_entry_new(clippor_entry_get_index(cur) + 1, source);

    while (
        g_hash_table_iter_next(&iter, (gpointer *)&mime_type, (gpointer *)&data)
    )
        clippor_entry_add_mime_type(entry, mime_type, data);

    g_queue_push_head(self->entries, entry);
}

static void
on_client_selection(GObject *client, GHashTable *mime_types, gpointer data)
{
    ClipporClipboard *cb = data;

    clippor_clipboard_new_entry(cb, mime_types, client);
}

static gboolean
on_client_removal_helper(gpointer key, gpointer value, gpointer data)
{
    ClipporClipboard *cb = ((void **)data)[0];
    GObject *client2rm = ((void **)data)[1];
    GObject *client = value;
    const gchar *label = key;

    if (client2rm == client)
    {
        g_debug("Removing client '%s' from clipboard '%s'", label, cb->label);
        return TRUE;
    }
    return FALSE;
}

/*
 * Called when either clipboard-regular or clipboard-primary property of client
 * is changed to a different clipboard.
 */
static void
on_client_removal(
    GObject *client, GParamSpec *pspec G_GNUC_UNUSED, gpointer data
)
{
    ClipporClipboard *cb = data;
    void *stuff[] = {cb, client};

    g_hash_table_foreach_remove(cb->clients, on_client_removal_helper, stuff);
}

/*
 * Returns FALSE if client with label is already added.
 */
gboolean
clippor_clipboard_add_client(
    ClipporClipboard *self, GObject *client, const gchar *label,
    ClipporSelectionType selection
)
{
    g_return_val_if_fail(CLIPPOR_IS_CLIPBOARD(self), FALSE);
    g_return_val_if_fail(client != NULL, FALSE);
    g_return_val_if_fail(label != NULL, FALSE);
    g_return_val_if_fail(selection != CLIPPOR_SELECTION_TYPE_NONE, FALSE);

    if (g_hash_table_contains(self->clients, client))
        return FALSE;

    g_hash_table_insert(self->clients, g_strdup(label), g_object_ref(client));

    const gchar *property_name, *detail, *signal_name;

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
    {
        property_name = "clipboard-regular";
        detail = "notify::clipboard-regular";
        signal_name = "selection::regular";
    }
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
    {
        property_name = "clipboard-primary";
        detail = "notify::clipboard-primary";
        signal_name = "selection::primary";
    }
    else
        // Shouldn't happen
        return FALSE;

    g_object_set(client, property_name, self, NULL);
    g_signal_connect(client, detail, G_CALLBACK(on_client_removal), self);
    g_signal_connect(
        client, signal_name, G_CALLBACK(on_client_selection), self
    );

    return TRUE;
}

/*
 * May return NULL
 */
ClipporEntry *
clippor_clipboard_get_entry(ClipporClipboard *self, guint64 index)
{
    g_return_val_if_fail(CLIPPOR_IS_CLIPBOARD(self), FALSE);

    return g_queue_peek_nth(self->entries, index);
}
