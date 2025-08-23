#include "clippor-entry.h"
#include "clippor-clipboard.h"
#include <glib-object.h>
#include <glib.h>
#include <stdint.h>

/*
 * Represents an entry that can be serialized and deserialized from and into the
 * database.
 */

struct _ClipporEntry
{
    GObject parent_instance;

    char *id;
    int64_t creation_time;
    int64_t last_used_time;
    gboolean starred;

    GHashTable *mime_types; // Each key is a mime type and the value is a GBytes
                            // object containing the data

    char *cb; // Label of clipboard
};

G_DEFINE_TYPE(ClipporEntry, clippor_entry, G_TYPE_OBJECT)

static void
clippor_entry_dispose(GObject *object)
{
    ClipporEntry *self = CLIPPOR_ENTRY(object);

    g_clear_pointer(&self->mime_types, g_hash_table_unref);

    G_OBJECT_CLASS(clippor_entry_parent_class)->dispose(object);
}

static void
clippor_entry_finalize(GObject *object)
{
    ClipporEntry *self = CLIPPOR_ENTRY(object);

    g_free(self->id);
    g_free(self->cb);

    G_OBJECT_CLASS(clippor_entry_parent_class)->finalize(object);
}

static void
clippor_entry_class_init(ClipporEntryClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->dispose = clippor_entry_dispose;
    gobject_class->finalize = clippor_entry_finalize;
}

static void
clippor_entry_init(ClipporEntry *self)
{
    self->mime_types = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref
    );
}

ClipporEntry *
clippor_entry_new_full(
    const char *cb_label, const char *id, int64_t creation_time,
    int64_t last_used_time, gboolean starred
)
{
    g_assert(cb_label != NULL);
    g_assert(id != NULL);
    g_assert(creation_time >= 0);
    g_assert(last_used_time >= 0);

    ClipporEntry *entry = g_object_new(CLIPPOR_TYPE_ENTRY, NULL);

    entry->cb = g_strdup(cb_label);
    entry->id = g_strdup(id);
    entry->creation_time = creation_time;
    entry->last_used_time = last_used_time;
    entry->starred = starred;

    return entry;
}

ClipporEntry *
clippor_entry_new(ClipporClipboard *cb)
{
    g_assert(cb == NULL || CLIPPOR_IS_CLIPBOARD(cb));

    const char *label = cb != NULL ? clippor_clipboard_get_label(cb) : "";
    int64_t creation_time = g_get_real_time();

    // Id is a checksum of creation_time and clipboard label
    g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA1);

    g_checksum_update(
        checksum, (uint8_t *)&creation_time, sizeof(creation_time)
    );
    g_checksum_update(checksum, (uint8_t *)label, strlen(label));

    const char *id = g_checksum_get_string(checksum);

    return clippor_entry_new_full(
        label, id, creation_time, creation_time, FALSE
    );
}

static gboolean
compare_mime_type_data(void *key G_GNUC_UNUSED, void *value, void *user_data)
{
    return g_bytes_compare(value, user_data) == 0;
}

void
clippor_entry_add_mime_type(
    ClipporEntry *self, const char *mime_type, GBytes *data
)
{
    g_assert(CLIPPOR_IS_ENTRY(self));
    g_assert(mime_type != NULL);
    g_assert(data != NULL);

    // Check if mime type with same data already exists, if so then use that
    // instead.
    GBytes *bytes =
        g_hash_table_find(self->mime_types, compare_mime_type_data, data);

    if (bytes == NULL)
        bytes = data;

    g_hash_table_insert(
        self->mime_types, g_strdup(mime_type), g_bytes_ref(bytes)
    );
}

GHashTable *
clippor_entry_get_mime_types(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->mime_types;
}

GBytes *
clippor_entry_get_data(ClipporEntry *self, const char *mime_type)
{
    g_assert(CLIPPOR_IS_ENTRY(self));
    g_assert(mime_type != NULL);

    return g_hash_table_lookup(self->mime_types, mime_type);
}

const char *
clippor_entry_get_clipboard(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->cb;
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

gboolean
clippor_entry_is_starred(ClipporEntry *self)
{
    g_assert(CLIPPOR_IS_ENTRY(self));

    return self->starred;
}
