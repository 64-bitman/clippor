#include "clippor-clipboard.h"
#include "clippor-client.h"
#include "clippor-entry.h"
#include "database.h"
#include "dbus-service.h"
#include "global.h"
#include <glib-object.h>
#include <inttypes.h>
#include <stdint.h>

G_DEFINE_ENUM_TYPE(
    ClipporSelectionType, clippor_selection_type,
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_NONE, "none"),
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_REGULAR, "regular"),
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_PRIMARY, "primary")
)

struct _ClipporClipboard
{
    GObject parent;

    char *label;

    // Each key is a label with the value being a weak pointer to the client
    // object.
    GHashTable *clients;

    int64_t max_entries;
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

static GParamSpec *obj_properties[N_PROPERTIES] = {NULL};

static void callback_client_unref(gpointer weak_ref);

static void
clippor_clipboard_set_property(
    GObject *object, uint property_id, const GValue *value, GParamSpec *pspec
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
        self->max_entries = g_value_get_int64(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
clippor_clipboard_get_property(
    GObject *object, uint property_id, GValue *value, GParamSpec *pspec
)
{
    ClipporClipboard *self = CLIPPOR_CLIPBOARD(object);

    switch ((ClipporClipboardProperty)property_id)
    {
    case PROP_LABEL:
        g_value_set_string(value, self->label);
        break;
    case PROP_MAX_ENTRIES:
        g_value_set_int64(value, self->max_entries);
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
    obj_properties[PROP_MAX_ENTRIES] = g_param_spec_int64(
        "max-entries", "Max entries",
        "Maximum amount of entries stored in memory", 1, G_MAXINT64, 10,
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
clippor_clipboard_new(const char *label)
{
    g_assert(label != NULL);

    ClipporClipboard *cb =
        g_object_new(CLIPPOR_TYPE_CLIPBOARD, "label", label, NULL);

    // Load entries from database
    ClipporEntry *entry;
    GError *error = NULL;

    for (int64_t i = 0; i < cb->max_entries; i++)
    {
        entry = database_deserialize_entry(cb, i, NULL, &error);

        if (entry == NULL)
        {
            // Either we got an error or there are no more entries left
            if (error != NULL)
            {
                // Dont message an error if database is just empty
                if (error->code != DATABASE_ERROR_ROW_NONEXISTENT)
                    g_message(
                        "Failed loading entry for clipboard '%s': %s",
                        cb->label, error->message
                    );
                g_error_free(error);
            }
            break;
        }
        g_queue_push_tail(cb->entries, entry);
    }

    dbus_service_add_clipboard(cb);

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

gboolean
clippor_clipboard_add_entry(
    ClipporClipboard *self, ClipporEntry *entry, GError **error
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    // Shove out excess old entries until there is one spot available
    while (self->entries->length >= self->max_entries)
        g_object_unref(g_queue_pop_tail(self->entries));

    g_queue_push_head(self->entries, entry);

    // Add to database
    return database_add_entry(entry, error);
}

/*
 * Called when there is a new selection from a client. If so then update the
 * other clients and add a new entry to the queue.
 */
static void
callback_client_selection(
    GObject *object, ClipporSelectionType selection, gpointer data
)
{
    ClipporClient *client = CLIPPOR_CLIENT(object);
    ClipporClipboard *cb = data;

    ClipporEntry *entry = clippor_entry_new(object, -1, NULL, cb);

    // Get mime types & data and add them to the entry
    GPtrArray *mime_types = clippor_client_get_mime_types(client, selection);
    GError *error = NULL;

    // Abort on any error
    for (uint i = 0; i < mime_types->len; i++)
    {
        const char *mime_type = mime_types->pdata[i];

        // Will be NULL during unit tests
        if (ALLOWED_MIME_TYPES == NULL)
            goto allowed;

        // Check if mime type is allowed
        for (uint k = 0; k < ALLOWED_MIME_TYPES->len; k++)
        {
            GRegex *regex = ALLOWED_MIME_TYPES->pdata[k];

            if (g_regex_match(regex, mime_type, G_REGEX_MATCH_DEFAULT, NULL))
                goto allowed;
        }
        continue;
allowed:;
        GBytes *data =
            clippor_client_get_data(client, mime_type, selection, &error);

        if (data == NULL)
        {
            g_assert(error != NULL);

            g_message(
                "Failed receiving data from client for clipboard '%s': %s",
                cb->label, error->message
            );

            g_error_free(error);
            g_object_unref(entry);
            g_ptr_array_unref(mime_types);
            return;
        }

        if (MIME_TYPE_GROUPS == NULL)
            goto skip;
        // Check if mime type has a group. If so then also add the other mime
        // types in the group with the same GBytes object.
        GHashTableIter iter;
        GRegex *regex;
        GPtrArray *group_mimes;

        g_hash_table_iter_init(&iter, MIME_TYPE_GROUPS);

        while (g_hash_table_iter_next(
            &iter, (gpointer *)&regex, (gpointer *)&group_mimes
        ))
            if (g_regex_match(regex, mime_type, G_REGEX_MATCH_DEFAULT, NULL))
            {
                // Add mime types
                for (uint k = 0; k < group_mimes->len; k++)
                    clippor_entry_add_mime_type(
                        entry, group_mimes->pdata[k], data
                    );
            }
skip:
        clippor_entry_add_mime_type(entry, mime_type, data);
        g_bytes_unref(data);
    }

    g_ptr_array_unref(mime_types);

    if (!clippor_clipboard_add_entry(cb, entry, &error))
    {
        g_message(
            "Failed adding entry for clipboard '%s': %s", cb->label,
            error->message
        );
        g_error_free(error);
        return;
    }

    // Update clients (includes the source client too, because we need to keep
    // their current entry up to date)
    GHashTableIter iter;
    GWeakRef *weak_ref;

    g_hash_table_iter_init(&iter, cb->clients);

    while (g_hash_table_iter_next(&iter, NULL, (gpointer *)&weak_ref))
    {
        ClipporClient *client = g_weak_ref_get(weak_ref);

        if (client == NULL)
            // Client was finalized, remove it
            g_hash_table_iter_remove(&iter);

        // Set the respective entry to the current one
        clippor_client_set_entry(client, entry, selection, &error);
    }
}

void
clippor_clipboard_add_client(
    ClipporClipboard *self, const char *label, ClipporClient *client,
    ClipporSelectionType selection
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));
    g_assert(G_IS_OBJECT(client));
    g_assert(label != NULL);

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

    ClipporEntry *entry = g_queue_peek_head(self->entries);
    GError *error = NULL;

    if (entry != NULL)
        clippor_client_set_entry(client, entry, selection, &error);

    char *signal_name;

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        signal_name = "selection::regular";
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        signal_name = "selection::primary";
    else
        return;

    // Start listening for new selections (for that specific selection)
    g_signal_connect(
        client, signal_name, G_CALLBACK(callback_client_selection), self
    );
}

/*
 * May return NULL on error or if there is no such entry
 */
ClipporEntry *
clippor_clipboard_get_entry(
    ClipporClipboard *self, int64_t index, GError **error
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));

    ClipporEntry *entry;

    // If index is outside the in memory list, search the database
    if (index > self->max_entries)
        entry = database_deserialize_entry(self, index, NULL, error);
    else
    {
        entry = g_queue_peek_nth(self->entries, index);

        if (entry == NULL)
            g_set_error(
                error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
                "No such entry with index %" PRIu64 " in history", index
            );

        if (entry != NULL)
            g_object_ref(entry);
    }

    if (entry == NULL)
        g_assert(error == NULL || *error != NULL);

    return entry;
}

ClipporEntry *
clippor_clipboard_get_entry_by_id(
    ClipporClipboard *self, const char *id, GError **error
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));
    g_assert(id != NULL);

    ClipporEntry *entry = database_deserialize_entry(self, 0, id, error);

    if (entry == NULL)
        g_assert(error == NULL || *error != NULL);

    return entry;
}

const char *
clippor_clipboard_get_label(ClipporClipboard *self)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));

    return self->label;
}

int64_t
clippor_clipboard_get_max_entries(ClipporClipboard *self)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(self));

    return self->max_entries;
}
