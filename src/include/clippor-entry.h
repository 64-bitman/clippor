#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#define CLIPPOR_TYPE_ENTRY (clippor_entry_get_type())

G_DECLARE_FINAL_TYPE(ClipporEntry, clippor_entry, CLIPPOR, ENTRY, GObject)

ClipporEntry *clippor_entry_new(guint64 index, GObject *source_client);

guint64 clippor_entry_get_index(ClipporEntry *self);

GObject *clippor_entry_get_source(ClipporEntry *self);

GHashTable *clippor_entry_get_mime_types(ClipporEntry *self);

gboolean
clippor_entry_set_file(ClipporEntry *self, GFile *file, GError **error);

void clippor_entry_add_mime_type(
    ClipporEntry *self, const gchar *mime_type, GBytes *data
);
