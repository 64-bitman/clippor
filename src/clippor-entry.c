#include "clippor-entry.h"
#include "clippor-clipboard.h"
#include "database.h"
#include <gio/gio.h>
#include <glib-object.h>

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

    GObject *from; // Which client this entry is from, NULL if unknown
};

G_DEFINE_TYPE(ClipporEntry, clippor_entry, G_TYPE_OBJECT)

typedef enum
{
    PROP_INDEX = 1, // 0 is used on error
    PROP_MIME_TYPES,
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
    case PROP_INDEX:;
        GError *error = NULL;
        int64_t val = database_get_entry_index(self, &error);

        if (val == -1)
        {
            g_assert(error != NULL);

            g_message("Failed getting index for entry: %s", error->message);
            g_error_free(error);
            val = 0;
        }
        g_value_set_int64(value, val);
        break;
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

    obj_properties[PROP_INDEX] = g_param_spec_int64(
        "index", "Index", "Index in history", 0, G_MAXINT64, 0, G_PARAM_READABLE
    );
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
        g_str_hash, g_str_equal, g_free, (void (*)(void *))g_bytes_unref
    );
}

/*
 * If a negative number is passed for creation_time, the current time is used.
 * If NULL is passed for id then it is auto generated.
 */
ClipporEntry *
clippor_entry_new(
    GObject *from, int64_t creation_time, const char *id,
    ClipporClipboard *parent
)
{
    ClipporEntry *entry = g_object_new(CLIPPOR_TYPE_ENTRY, NULL);

    entry->from = from;
    entry->cb = parent;

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

GHashTable *
clippor_entry_get_mime_types(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->mime_types;
}

GObject *
clippor_entry_is_from(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->from;
}

/*
 * Adds mime type to entry along with its data (can be NULL).
 */
void
clippor_entry_add_mime_type(
    ClipporEntry *self, const char *mime_type, GBytes *data
)
{
    g_assert(CLIPPOR_IS_ENTRY(self));
    g_assert(mime_type != NULL);

    if (data != NULL)
        g_bytes_ref(data);

    g_hash_table_insert(self->mime_types, g_strdup(mime_type), data);
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
 * Returns a new reference
 */
GBytes *
clippor_entry_get_data(
    ClipporEntry *self, const char *mime_type, GError **error
)
{
    g_assert(CLIPPOR_IS_ENTRY(self));
    g_assert(mime_type != NULL);

    // If we haven't loaded in the data yet from the database, do it now.
    GBytes *data = g_hash_table_lookup(self->mime_types, mime_type);

    if (data == NULL)
    {
        data = database_deserialize_mime_type(self, mime_type, error);

        if (data == NULL)
            return NULL;

        g_hash_table_insert(self->mime_types, g_strdup(mime_type), data);
        return g_bytes_ref(data);
    }

    return g_bytes_ref(g_hash_table_lookup(self->mime_types, mime_type));
}

ClipporClipboard *
clippor_entry_get_clipboard(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->cb;
}
