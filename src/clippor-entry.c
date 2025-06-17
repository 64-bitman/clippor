#include "clippor-entry.h"
#include <gio/gio.h>
#include <glib-object.h>

struct _ClipporEntry
{
    GObject parent;

    guint64 index; // Index in the history list

    GFile *file; // Persistent file that stores the data

    GHashTable *mime_types; // Hash table of mime types. Each value is a pointer
                            // Bytes GObject of the data it represents. Value
                            // may be NULL is save memory. If there are no mime
                            // types then assume selection was cleared/empty.

    GObject *source; // Source client of this entry. Used to prevent a
                     // client from trying to set its selection when it
                     // was the one that set the selection.
};

G_DEFINE_TYPE(ClipporEntry, clippor_entry, G_TYPE_OBJECT)

typedef enum
{
    PROP_INDEX = 1, // Starts from zero
    PROP_MIME_TYPES,
    PROP_FILE, // Where data is stored persistently
    PROP_SOURCE,
    N_PROPERTIES
} ClipporEntryProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {
    NULL,
};

static void
clippor_entry_set_property(
    GObject *object, guint property_id, const GValue *value, GParamSpec *pspec
)
{
    ClipporEntry *self = CLIPPOR_ENTRY(object);

    switch ((ClipporEntryProperty)property_id)
    {
    case PROP_INDEX:
        self->index = g_value_get_uint64(value);
        break;
    case PROP_SOURCE:
        if (self->source != NULL)
            g_object_unref(self->source);
        self->source = g_value_dup_object(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
clippor_entry_get_property(
    GObject *object, guint property_id, GValue *value, GParamSpec *pspec
)
{
    ClipporEntry *self = CLIPPOR_ENTRY(object);

    switch ((ClipporEntryProperty)property_id)
    {
    case PROP_INDEX:
        g_value_set_uint64(value, self->index);
        break;
    case PROP_FILE:
        g_value_set_object(value, self->file);
        break;
    case PROP_MIME_TYPES:
        g_value_set_boxed(value, self->mime_types);
        break;
    case PROP_SOURCE:
        g_value_set_object(value, self->source);
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

    g_clear_object(&self->file);
    g_clear_object(&self->source);
    g_hash_table_remove_all(self->mime_types);

    G_OBJECT_CLASS(clippor_entry_parent_class)->dispose(object);
}

static void
clippor_entry_finalize(GObject *object)
{
    ClipporEntry *self G_GNUC_UNUSED = CLIPPOR_ENTRY(object);

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

    obj_properties[PROP_INDEX] = g_param_spec_uint64(
        "index", "Index", "Position in the history list", 0, G_MAXUINT64, 0,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    );
    obj_properties[PROP_MIME_TYPES] = g_param_spec_boxed(
        "mime-types", "Mime types",
        "List of mime types that can be represented", G_TYPE_HASH_TABLE,
        G_PARAM_READABLE
    );
    obj_properties[PROP_FILE] = g_param_spec_object(
        "file", "File", "Persistent file holding all data", G_TYPE_FILE,
        G_PARAM_READABLE
    );
    obj_properties[PROP_SOURCE] = g_param_spec_object(
        "source", "Source", "Source client of entry", G_TYPE_OBJECT,
        G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
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

ClipporEntry *
clippor_entry_new(guint64 index, GObject *source_client)
{
    // If index is 0 (NULL) the default value will set it to 0
    ClipporEntry *entry = g_object_new(
        CLIPPOR_TYPE_ENTRY, "index", index, "source", source_client, NULL
    );

    return entry;
}

guint64
clippor_entry_get_index(ClipporEntry *self)
{
    g_return_val_if_fail(CLIPPOR_IS_ENTRY(self), 0);

    guint64 index;

    g_object_get(self, "index", &index, NULL);

    return index;
}

GObject *
clippor_entry_get_source(ClipporEntry *self)
{
    g_return_val_if_fail(CLIPPOR_IS_ENTRY(self), 0);

    return self->source;
}

GHashTable *
clippor_entry_get_mime_types(ClipporEntry *self)
{
    g_return_val_if_fail(CLIPPOR_IS_ENTRY(self), 0);

    return self->mime_types;
}

/*
 * File must not be NULL
 */
gboolean
clippor_entry_set_file(ClipporEntry *self, GFile *file, GError **error)
{
    g_return_val_if_fail(CLIPPOR_IS_ENTRY(self), FALSE);
    g_return_val_if_fail(G_IS_FILE(file), FALSE);
    g_return_val_if_fail(self->file == NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

    (void)error;

    self->file = g_object_ref(file);
    return TRUE;
}

/*
 * Mime type must not be NULL
 */
void
clippor_entry_add_mime_type(
    ClipporEntry *self, const gchar *mime_type, GBytes *data
)
{
    g_return_if_fail(CLIPPOR_IS_ENTRY(self));
    g_return_if_fail(mime_type != NULL);

    g_hash_table_insert(
        self->mime_types, g_strdup(mime_type), g_bytes_ref(data)
    );
}
