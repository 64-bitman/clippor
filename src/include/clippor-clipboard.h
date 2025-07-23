#pragma once

#include "clippor-database.h"
#include "clippor-selection.h"
#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(
    ClipporClipboard, clippor_clipboard, CLIPPOR, CLIPBOARD, GObject
)
#define CLIPPOR_TYPE_CLIPBOARD (clippor_clipboard_get_type())

typedef enum
{
    CLIPPOR_CLIPBOARD_ERROR_RECEIVE,
} ClipporClipboardError;

ClipporClipboard *clippor_clipboard_new(const char *label);

gboolean clippor_clipboard_set_database(
    ClipporClipboard *self, ClipporDatabase *db, GError **error
);

void
clippor_clipboard_add_selection(ClipporClipboard *self, ClipporSelection *sel);

const char *clippor_clipboard_get_label(ClipporClipboard *self);
