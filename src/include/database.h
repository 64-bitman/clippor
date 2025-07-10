#pragma once

#include "clippor-clipboard.h"
#include "clippor-entry.h"
#include <glib.h>

#define DATABASE_ERROR (database_error_quark())

typedef enum
{
    DATABASE_ERROR_NO_DATA_DIR,
    DATABASE_ERROR_SQLITE_EXEC,
    DATABASE_ERROR_SQLITE_PREPARE,
    DATABASE_ERROR_SQLITE_STEP,
    DATABASE_ERROR_SQLITE_OPEN,
    DATABASE_ERROR_ROW_NONEXISTENT,
    DATABASE_ERROR_ADD,
    DATABASE_ERROR_WRITE
} DatabaseError;

GQuark database_error_quark(void);

gboolean database_init(GError **error);
void database_uninit(void);

gboolean database_new_entry_row(ClipporEntry *entry, GError **error);
gboolean database_update_entry_row(ClipporEntry *entry, GError **error);

gboolean database_new_mime_type_row(
    ClipporEntry *entry, const char *mime_type, ClipporData *data,
    GError **error
);

ClipporEntry *database_get_entry_by_position(
    ClipporClipboard *cb, int64_t index, GError **error
);
ClipporEntry *
database_get_entry_by_id(ClipporClipboard *cb, const char *id, GError **error);

ClipporData *database_get_entry_mime_type_data(
    ClipporEntry *entry, const char *mime_type, GError **error
);

gboolean database_remove_mime_type_row_by_id(
    const char *id, const char *mime_type, GError **error
);

gboolean database_remove_entry_row_by_id(const char *id, GError **error);

gboolean
database_trim_entry_rows(ClipporClipboard *cb, gboolean all, GError **error);

gboolean database_update_mime_type_row(
    ClipporEntry *entry, const char *mime_type, ClipporData *data,
    GError **error
);

int64_t database_get_num_entries(ClipporClipboard *cb, GError **error);

int database_entry_id_exists(const char *id, GError **error);
