#pragma once

#include "clippor-clipboard.h"
#include "util.h"
#include <gio/gio.h>
#include <glib-object.h>

#define CLIPPOR_TYPE_ENTRY (clippor_entry_get_type())

G_DECLARE_FINAL_TYPE(ClipporEntry, clippor_entry, CLIPPOR, ENTRY, GObject)

#define CLIPPOR_ENTRY_ERROR (clippor_entry_error_quark())

typedef enum
{
    CLIPPOR_ENTRY_ERROR_NO_MIME_TYPE,
    CLIPPOR_ENTRY_ERROR_UPDATE
} ClipporEntryError;

GQuark clippor_entry_error_quark(void);

ClipporEntry *clippor_entry_new_no_database(
    ClipporClient *from, int64_t creation_time, const char *id,
    ClipporClipboard *parent, ClipporSelectionType selection
);
ClipporEntry *clippor_entry_new(
    ClipporClient *from, int64_t creation_time, const char *id,
    ClipporClipboard *parent, ClipporSelectionType selection, GError **error
);

GHashTable *clippor_entry_get_mime_types(ClipporEntry *self);

ClipporClient *clippor_entry_is_from(ClipporEntry *self);

ClipporSelectionType clippor_entry_get_selection(ClipporEntry *self);

gboolean clippor_entry_set_mime_type_no_database(
    ClipporEntry *self, const char *mime_type, ClipporData *data
);
gboolean clippor_entry_set_mime_type(
    ClipporEntry *self, const char *mime_type, ClipporData *data, GError **error
);

gboolean clippor_entry_has_mime_type(ClipporEntry *self, const char *mime_type);

gboolean clippor_entry_is_starred(ClipporEntry *self);

int64_t clippor_entry_get_creation_time(ClipporEntry *self);
int64_t clippor_entry_get_last_used_time(ClipporEntry *self);
const char *clippor_entry_get_id(ClipporEntry *self);

gboolean clippor_entry_update_property(
    ClipporEntry *self, GError **error, const char *property, ...
);
gboolean clippor_entry_update_last_used(ClipporEntry *self, GError **error);

ClipporData *clippor_entry_get_data(
    ClipporEntry *self, const char *mime_type, GError **error
);

ClipporClipboard *clippor_entry_get_clipboard(ClipporEntry *self);
