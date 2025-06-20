#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#define CLIPPOR_TYPE_ENTRY (clippor_entry_get_type())

G_DECLARE_FINAL_TYPE(ClipporEntry, clippor_entry, CLIPPOR, ENTRY, GObject)

ClipporEntry *clippor_entry_new(guint64 index, GObject *from);

guint64 clippor_entry_get_index(ClipporEntry *self);

GHashTable *clippor_entry_get_mime_types(ClipporEntry *self);

GObject *clippor_entry_is_from(ClipporEntry *self);

void clippor_entry_add_mime_type(
    ClipporEntry *self, const gchar *mime_type, GBytes *data
);

void clippor_entry_set_mime_types(
    ClipporEntry *self, GHashTable *mime_types, gboolean take
);
