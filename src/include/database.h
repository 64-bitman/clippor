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

gboolean database_add_entry(ClipporEntry *entry, GError **error);

ClipporEntry *database_deserialize_entry(
    ClipporClipboard *cb, int64_t index, const char *id, GError **error
);

GBytes *database_deserialize_mime_type(
    ClipporEntry *entry, const char *mime_type, GError **error
);

int64_t database_get_num_entries(ClipporClipboard *cb, GError **error);
int64_t database_get_entry_index(ClipporEntry *entry, GError **error);

int database_entry_id_exists(const char *id, GError **error);

gboolean
database_trim_entries(ClipporClipboard *cb, gboolean all, GError **error);

gboolean database_remove_id(const char *id, GError **error);
