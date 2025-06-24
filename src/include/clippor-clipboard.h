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

typedef struct _ClipporClient ClipporClient;

GType clippor_selection_type_get_type(void);

ClipporClipboard *clippor_clipboard_new(const gchar *label);

void clippor_clipboard_add_entry(ClipporClipboard *self, ClipporEntry *entry);

void clippor_clipboard_add_client(
    ClipporClipboard *self, const char *label, ClipporClient *client,
    ClipporSelectionType selection
);

ClipporEntry *clippor_clipboard_get_entry(ClipporClipboard *self, guint64 index);
