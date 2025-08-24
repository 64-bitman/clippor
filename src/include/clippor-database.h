#pragma once

#include "clippor-entry.h"
#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(
    ClipporDatabase, clippor_database, CLIPPOR, DATABASE, GObject
)
#define CLIPPOR_TYPE_DATABASE (clippor_database_get_type())

typedef enum
{
    CLIPPOR_DATABASE_ERROR_OPEN,
    CLIPPOR_DATABASE_ERROR_EXEC,
    CLIPPOR_DATABASE_ERROR_PREPARE,
    CLIPPOR_DATABASE_ERROR_STEP,
    CLIPPOR_DATABASE_ERROR_DATA_DIR,
    CLIPPOR_DATABASE_ERROR_ROW_NOT_EXIST,
    CLIPPOR_DATABASE_ERROR_FAILED,
} ClipporDatabaseError;

#define CLIPPOR_DATABASE_ERROR (clippor_database_error_quark())
GQuark clippor_database_error_quark(void);

typedef enum
{
    CLIPPOR_DATABASE_DEFAULT = 0,
    CLIPPOR_DATABASE_IN_MEMORY = 1 << 0
} ClipporDatabaseBitFlags;

typedef uint32_t ClipporDatabaseFlags;

ClipporDatabase *
clippor_database_new(const char *data_directory, uint flags, GError **error);

int clippor_database_entry_exists(
    ClipporDatabase *self, ClipporEntry *entry, GError **error
);

gboolean clippor_database_serialize_entry(
    ClipporDatabase *self, ClipporEntry *entry, GError **error
);

ClipporEntry *clippor_database_deserialize_entry_at_index(
    ClipporDatabase *self, const char *cb, int64_t index, GError **error
);
ClipporEntry *clippor_database_deserialize_entry_with_id(
    ClipporDatabase *self, const char *id, GError **error
);
GPtrArray *clippor_database_deserialize_entries(
    ClipporDatabase *self, int64_t start, int64_t end, GError **error
);
gboolean clippor_database_trim_entries(
    ClipporDatabase *self, const char *cb, int64_t n, GError **error
);
