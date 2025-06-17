#pragma once

#include "clippor-entry.h"
#include <glib-object.h>

#define CLIPPOR_TYPE_CLIPBOARD (clippor_clipboard_get_type())

G_DECLARE_FINAL_TYPE(
    ClipporClipboard, clippor_clipboard, CLIPPOR, CLIPBOARD, GObject
)

#define CLIPPOR_TYPE_SELECTION_TYPE (clippor_selection_type_get_type())

typedef enum
{
    CLIPPOR_SELECTION_TYPE_NONE = 0,
    CLIPPOR_SELECTION_TYPE_REGULAR = 1 << 0,
    CLIPPOR_SELECTION_TYPE_PRIMARY = 1 << 1
} ClipporSelectionType;

GType clippor_selection_type_get_type(void);

ClipporClipboard *clippor_clipboard_new(const gchar *label);

void clippor_clipboard_update_history(ClipporClipboard *self);
void clippor_clipboard_new_entry(
    ClipporClipboard *self, GHashTable *mime_types, GObject *source
);

gboolean clippor_clipboard_add_client(
    ClipporClipboard *self, GObject *client, const gchar *label,
    const gchar *property_name, const gchar *signal_name
);

ClipporEntry *
clippor_clipboard_get_entry(ClipporClipboard *self, guint64 index);
