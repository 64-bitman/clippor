#pragma once

#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(ClipporEntry, clippor_entry, CLIPPOR, ENTRY, GObject)
#define CLIPPOR_TYPE_ENTRY (clippor_entry_get_type())

typedef struct _ClipporClipboard ClipporClipboard;

ClipporEntry *clippor_entry_new_full(
    const char *cb_label, const char *id, int64_t creation_time,
    int64_t last_used_time, gboolean starred
);
ClipporEntry *clippor_entry_new(ClipporClipboard *cb);

void clippor_entry_add_mime_type(
    ClipporEntry *self, const char *mime_type, GBytes *data
);

GHashTable *clippor_entry_get_mime_types(ClipporEntry *self);
GBytes *clippor_entry_get_data(ClipporEntry *self, const char *mime_type);
const char *clippor_entry_get_clipboard(ClipporEntry *self);
int64_t clippor_entry_get_creation_time(ClipporEntry *self);
int64_t clippor_entry_get_last_used_time(ClipporEntry *self);
const char *clippor_entry_get_id(ClipporEntry *self);
gboolean clippor_entry_is_starred(ClipporEntry *self);
