#pragma once

#include "clippor-clipboard.h"
#include <gio/gio.h>
#include <glib-object.h>

#define CLIPPOR_TYPE_ENTRY (clippor_entry_get_type())

G_DECLARE_FINAL_TYPE(ClipporEntry, clippor_entry, CLIPPOR, ENTRY, GObject)

ClipporEntry *clippor_entry_new(
    ClipporClient *from, int64_t creation_time, const char *id,
    ClipporClipboard *parent, ClipporSelectionType selection
);

GHashTable *clippor_entry_get_mime_types(ClipporEntry *self);

ClipporClient *clippor_entry_is_from(ClipporEntry *self);

ClipporSelectionType clippor_entry_get_selection(ClipporEntry *self);

void clippor_entry_add_mime_type(
    ClipporEntry *self, const char *mime_type, GBytes *data
);
gboolean clippor_entry_has_mime_type(ClipporEntry *self, const char *mime_type);

gboolean clippor_entry_is_starred(ClipporEntry *self);

int64_t clippor_entry_get_creation_time(ClipporEntry *self);
int64_t clippor_entry_get_last_used_time(ClipporEntry *self);
const char *clippor_entry_get_id(ClipporEntry *self);

GBytes *clippor_entry_get_data(
    ClipporEntry *self, const char *mime_type, GError **error
);

ClipporClipboard *clippor_entry_get_clipboard(ClipporEntry *self);
