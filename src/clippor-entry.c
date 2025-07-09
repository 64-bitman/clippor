#include "clippor-entry.h"
#include "clippor-client.h"
#include "clippor-clipboard.h"
#include "database.h"
#include "util.h"
#include <gio/gio.h>
#include <glib-object.h>

G_DEFINE_QUARK(clippor_entry_error_quark, clippor_entry_error)

struct _ClipporEntry
{
    GObject parent;

    ClipporClipboard *cb; // Parent clipboard

    GHashTable *mime_types; // Hash table of mime types. Each value is a pointer
                            // Bytes GObject of the data it represents. Value
                            // may be NULL is save memory.
    gboolean starred;
    char *id; // Set when entry was created from the database

    int64_t creation_time;  // In microseconds
    int64_t last_used_time; // In microseconds

    ClipporClient *from; // Which client this entry is from, NULL if unknown
    ClipporSelectionType selection; // Which selection this entry is based on,
                                    // CLIPPOR_SELECTION_TYPE_NONE if unknown
};

G_DEFINE_TYPE(ClipporEntry, clippor_entry, G_TYPE_OBJECT)

typedef enum
{
    PROP_MIME_TYPES = 1,
    PROP_STARRED,
    PROP_ID,
    PROP_CREATION_TIME,
    PROP_LAST_USED_TIME,
    N_PROPERTIES
} ClipporEntryProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {NULL};

static void
clippor_entry_set_property(
    GObject *object, uint property_id, const GValue *value, GParamSpec *pspec
)
{
    ClipporEntry *self = CLIPPOR_ENTRY(object);

    switch ((ClipporEntryProperty)property_id)
    {
    case PROP_STARRED:
        self->starred = g_value_get_boolean(value);
        break;
    case PROP_LAST_USED_TIME:
        self->last_used_time = g_value_get_int64(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
clippor_entry_get_property(
    GObject *object, uint property_id, GValue *value, GParamSpec *pspec
)
{
    ClipporEntry *self = CLIPPOR_ENTRY(object);

    switch ((ClipporEntryProperty)property_id)
    {
    case PROP_MIME_TYPES:
        g_value_set_boxed(value, self->mime_types);
        break;
    case PROP_STARRED:
        g_value_set_boolean(value, self->starred);
        break;
    case PROP_ID:
        g_value_set_string(value, self->id);
        break;
    case PROP_CREATION_TIME:
        g_value_set_int64(value, self->creation_time);
        break;
    case PROP_LAST_USED_TIME:
        g_value_set_int64(value, self->last_used_time);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
clippor_entry_dispose(GObject *object)
{
    ClipporEntry *self = CLIPPOR_ENTRY(object);

    g_hash_table_remove_all(self->mime_types);

    G_OBJECT_CLASS(clippor_entry_parent_class)->dispose(object);
}

static void
clippor_entry_finalize(GObject *object)
{
    ClipporEntry *self G_GNUC_UNUSED = CLIPPOR_ENTRY(object);

    g_free(self->id);
    g_hash_table_unref(self->mime_types);
    G_OBJECT_CLASS(clippor_entry_parent_class)->finalize(object);
}

static void
clippor_entry_class_init(ClipporEntryClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->set_property = clippor_entry_set_property;
    gobject_class->get_property = clippor_entry_get_property;

    gobject_class->dispose = clippor_entry_dispose;
    gobject_class->finalize = clippor_entry_finalize;

    obj_properties[PROP_MIME_TYPES] = g_param_spec_boxed(
        "mime-types", "Mime types",
        "List of mime types that can be represented", G_TYPE_HASH_TABLE,
        G_PARAM_READWRITE
    );
    obj_properties[PROP_STARRED] = g_param_spec_boolean(
        "starred", "Starred", "If entry is starred", FALSE,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT
    );
    obj_properties[PROP_ID] = g_param_spec_string(
        "id", "Id", "Id that identifies this entry", "", G_PARAM_READABLE
    );
    obj_properties[PROP_CREATION_TIME] = g_param_spec_int64(
        "creation-time", "Creation time", "Time this entry was created", 0,
        G_MAXINT64, 0, G_PARAM_READABLE
    );
    obj_properties[PROP_LAST_USED_TIME] = g_param_spec_int64(
        "last-used-time", "Last used time", "Time this entry was last used", 0,
        G_MAXINT64, 0, G_PARAM_READWRITE
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
    );
}

static void
clippor_entry_init(ClipporEntry *self)
{
    self->mime_types = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (void (*)(void *))clippor_data_unref
    );
}

/*
 * If a negative number is passed for creation_time, the current time is used.
 * If NULL is passed for id then it is auto generated.
 */
ClipporEntry *
clippor_entry_new_no_database(
    ClipporClient *from, int64_t creation_time, const char *id,
    ClipporClipboard *parent, ClipporSelectionType selection
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(parent));
    g_assert(from == NULL || CLIPPOR_IS_CLIENT(from));

    ClipporEntry *entry = g_object_new(CLIPPOR_TYPE_ENTRY, NULL);

    entry->from = from;
    entry->cb = parent;
    entry->selection = selection;

    if (creation_time < 0)
        entry->creation_time = entry->last_used_time = g_get_real_time();
    else
        entry->creation_time = entry->last_used_time = creation_time;

    if (id == NULL)
    {
        const char *cb_label = clippor_clipboard_get_label(parent);
        GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA1);

        // Feed creation time, and parent clipboard label as the hash
        g_checksum_update(
            checksum, (guchar *)&entry->creation_time,
            sizeof(entry->creation_time)
        );
        g_checksum_update(checksum, (guchar *)cb_label, strlen(cb_label));

        entry->id = g_strdup(g_checksum_get_string(checksum));

        g_checksum_free(checksum);
    }
    else
        entry->id = g_strdup(id);

    return entry;
}

/*
 * Same as clippor_entry_new_no_database except creates an entry in the database
 */
ClipporEntry *
clippor_entry_new(
    ClipporClient *from, int64_t creation_time, const char *id,
    ClipporClipboard *parent, ClipporSelectionType selection, GError **error
)
{
    g_assert(error == NULL || *error == NULL);

    ClipporEntry *entry = clippor_entry_new_no_database(
        from, creation_time, id, parent, selection
    );

    if (!database_new_entry_row(entry, error))
    {
        g_object_unref(entry);
        return NULL;
    }

    return entry;
}

GHashTable *
clippor_entry_get_mime_types(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->mime_types;
}

ClipporClient *
clippor_entry_is_from(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->from;
}

ClipporSelectionType
clippor_entry_get_selection(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->selection;
}

/*
 * Adds mime type to entry along with its data (can be NULL). Returns TRUE if
 * the mime type wasn't added to the entry already
 */
gboolean
clippor_entry_set_mime_type_no_database(
    ClipporEntry *self, const char *mime_type, ClipporData *data
)
{
    g_assert(CLIPPOR_IS_ENTRY(self));
    g_assert(mime_type != NULL);

    if (data != NULL)
        clippor_data_ref(data);

    return g_hash_table_replace(self->mime_types, g_strdup(mime_type), data);
}

/*
 * Same as clippor_entry_set_mime_type_no_database except creates a new database
 * entry for mime type, updates it if it already exists, or deletes it if data
 * is NULL.
 */
gboolean
clippor_entry_set_mime_type(
    ClipporEntry *self, const char *mime_type, ClipporData *data, GError **error
)
{
    g_assert(CLIPPOR_IS_ENTRY(self));
    g_assert(mime_type != NULL);
    g_assert(error == NULL || *error == NULL);

    gboolean new =
        clippor_entry_set_mime_type_no_database(self, mime_type, data);

    if (data == NULL)
        // Delete mime type
        g_hash_table_remove(self->mime_types, mime_type);

    // Mime type groups may contain the same mime type as the one from the
    // selection, if so don't try adding both to the database.
    if (new)
        return database_new_mime_type_row(self, mime_type, data, error);
    else
        // Mime type already exists, update database
        return database_update_mime_type_row(self, mime_type, data, error);
}

gboolean
clippor_entry_has_mime_type(ClipporEntry *self, const char *mime_type)
{
    g_assert(CLIPPOR_IS_ENTRY(self));
    g_assert(mime_type != NULL);

    return g_hash_table_contains(self->mime_types, mime_type);
}

gboolean
clippor_entry_is_starred(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->starred;
}

int64_t
clippor_entry_get_creation_time(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->creation_time;
}

int64_t
clippor_entry_get_last_used_time(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->last_used_time;
}

const char *
clippor_entry_get_id(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->id;
}

/*
 * Returns a new reference to the data, else NULL if mime type doesn't exist in
 * the entry or database.
 */
ClipporData *
clippor_entry_get_data(
    ClipporEntry *self, const char *mime_type, GError **error
)
{
    g_assert(CLIPPOR_IS_ENTRY(self));
    g_assert(mime_type != NULL);

    if (!g_hash_table_contains(self->mime_types, mime_type))
    {
        g_set_error(
            error, CLIPPOR_ENTRY_ERROR, CLIPPOR_ENTRY_ERROR_NO_MIME_TYPE,
            "Mime type '%s' does not exist", mime_type
        );
        return NULL;
    }

    // If we haven't loaded in the data yet from the database, do it now.
    ClipporData *data = g_hash_table_lookup(self->mime_types, mime_type);

    if (data == NULL)
    {
        data = database_get_entry_mime_type_data(self, mime_type, error);

        if (data == NULL)
            return NULL;

        g_hash_table_insert(self->mime_types, g_strdup(mime_type), data);
        return clippor_data_ref(data);
    }

    return clippor_data_ref(data);
}

ClipporClipboard *
clippor_entry_get_clipboard(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->cb;
}
