#include "clippor-database.h"
#include "clippor-entry.h"
#include <glib-object.h>
#include <glib-unix.h>
#include <glib.h>
#include <sqlite3.h>

G_DEFINE_QUARK(CLIPPOR_DATABASE_ERROR, clippor_database_error)

struct _ClipporDatabase
{
    GObject parent_instance;

    char *location_dir;
    char *location;
    sqlite3 *handle;
    uint flags;

    // Used to store the data in memory instead of inside a file if configured
    // to. Each key is a data id and its value is a GBytes.
    GHashTable *store;
};

G_DEFINE_TYPE(ClipporDatabase, clippor_database, G_TYPE_OBJECT);

static void
clippor_database_dispose(GObject *object)
{
    ClipporDatabase *self = CLIPPOR_DATABASE(object);

    g_clear_pointer(&self->store, g_hash_table_unref);

    G_OBJECT_CLASS(clippor_database_parent_class)->dispose(object);
}

static void
clippor_database_finalize(GObject *object)
{
    ClipporDatabase *self = CLIPPOR_DATABASE(object);

    sqlite3_close(self->handle);
    g_free(self->location);
    g_free(self->location_dir);

    G_OBJECT_CLASS(clippor_database_parent_class)->finalize(object);
}

static void
clippor_database_class_init(ClipporDatabaseClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->dispose = clippor_database_dispose;
    gobject_class->finalize = clippor_database_finalize;
}

static void
clippor_database_init(ClipporDatabase *self G_GNUC_UNUSED)
{
}

ClipporDatabase *
clippor_database_new(const char *data_directory, uint flags, GError **error)
{
    ClipporDatabase *db = g_object_new(CLIPPOR_TYPE_DATABASE, NULL);

    db->flags = flags;

    char *location = NULL, *location_dir = NULL;

    if (flags & CLIPPOR_DATABASE_IN_MEMORY)
        location = g_strdup(":memory:");
    else
    {
        if (data_directory == NULL)
        {
            const char *user_dir = g_get_user_data_dir();

            location = g_strdup_printf("%s/clippor/history.sqlite3", user_dir);
        }
        else
            location = g_strdup_printf("%s/history.sqlite3", data_directory);

        location_dir = g_path_get_dirname(location);

        if (g_mkdir_with_parents(location_dir, 0755) == -1)
        {
            g_set_error(
                error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_OPEN,
                "Failed creating database directory: %s", g_strerror(errno)
            );
            return FALSE;
        }
    }

    db->location = location;
    db->location_dir = location_dir;

    int ret = sqlite3_open(location, &db->handle);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_OPEN,
            "Failed opening database at '%s': %s", location,
            sqlite3_errmsg(db->handle)
        );
        g_object_unref(db);
        return NULL;
    }

    const char *statement =
        "PRAGMA foreign_keys = ON;"
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous = NORMAL;"
        "CREATE TABLE IF NOT EXISTS Entries ("
        "   Position INTEGER PRIMARY KEY AUTOINCREMENT,"
        "   Id CHAR(40) NOT NULL UNIQUE,"
        "   Creation_time INTEGER NOT NULL CHECK (Creation_time > 0),"
        "   Last_used_time INTEGER NOT NULL CHECK (Last_used_time > 0),"
        "   Starred BOOLEAN,"
        "   Clipboard TEXT NOT NULL"
        ");"
        ""
        "CREATE TABLE IF NOT EXISTS Mime_types ("
        "   Id CHAR(40),"
        "   Mime_type TEXT,"
        "   Data_id CHAR(40),"
        "   PRIMARY KEY (Id, Mime_type),"
        "   FOREIGN KEY (Id) REFERENCES Entries(Id) ON DELETE RESTRICT,"
        "   FOREIGN KEY (Data_id) REFERENCES Data(Data_id) ON DELETE RESTRICT"
        ");"
        ""
        "CREATE TABLE IF NOT EXISTS Data ("
        "   Data_id CHAR(40) PRIMARY KEY,"
        "   Ref_count INTEGER DEFAULT 1 CHECK (Ref_count >= 0)"
        ");"
        ""
        "CREATE TABLE IF NOT EXISTS Version ("
        "   Db_version INTEGER NOT NULL DEFAULT 0"
        ");";

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_FAILED,
            "Failed creaing function in database: %s",
            sqlite3_errmsg(db->handle)
        );
        g_object_unref(db);
        return NULL;
    }

    char *err_msg;
    ret = sqlite3_exec(db->handle, statement, NULL, NULL, &err_msg);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_EXEC,
            "Failed creaing tables in database: %s", err_msg
        );
        sqlite3_free(err_msg);
        g_object_unref(db);
        return NULL;
    }

    if (flags & CLIPPOR_DATABASE_IN_MEMORY)
        db->store = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref
        );

    return db;
}

#define EXEC_ERROR(r)                                                          \
    do                                                                         \
    {                                                                          \
        g_set_error(                                                           \
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_EXEC,        \
            "Failed execing statement '%s': %s", statement, err_msg            \
        );                                                                     \
        sqlite3_free(err_msg);                                                 \
        return r;                                                              \
    } while (FALSE)
#define PREPARE_ERROR(r)                                                       \
    do                                                                         \
    {                                                                          \
        g_set_error(                                                           \
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_PREPARE,     \
            "Failed preparing statement '%s': %s", statement,                  \
            sqlite3_errmsg(self->handle)                                       \
        );                                                                     \
        return r;                                                              \
    } while (FALSE)
#define STEP_ERROR(r)                                                          \
    do                                                                         \
    {                                                                          \
        g_set_error(                                                           \
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_STEP,        \
            "Failed stepping statement '%s': %s", statement,                   \
            sqlite3_errmsg(self->handle)                                       \
        );                                                                     \
        sqlite3_finalize(stmt);                                                \
        return r;                                                              \
    } while (FALSE)
#define EXEC(r)                                                                \
    do                                                                         \
    {                                                                          \
        ret = sqlite3_exec(self->handle, statement, NULL, NULL, &err_msg);     \
        if (ret != SQLITE_OK)                                                  \
            EXEC_ERROR(r);                                                     \
    } while (FALSE)
#define PREPARE(r)                                                             \
    do                                                                         \
    {                                                                          \
        ret = sqlite3_prepare_v2(self->handle, statement, -1, &stmt, 0);       \
        if (ret != SQLITE_OK)                                                  \
            PREPARE_ERROR(r);                                                  \
    } while (FALSE)
#define PREPARE2(r)                                                            \
    do                                                                         \
    {                                                                          \
        ret = sqlite3_prepare_v2(self->handle, statement2, -1, &stmt2, 0);     \
        if (ret != SQLITE_OK)                                                  \
            PREPARE_ERROR(r);                                                  \
    } while (FALSE)
#define STEP_NO_ROW(r)                                                         \
    do                                                                         \
    {                                                                          \
        ret = sqlite3_step(stmt);                                              \
        if (ret != SQLITE_OK)                                                  \
            STEP_ERROR(r);                                                     \
        sqlite3_finalize(stmt);                                                \
    } while (FALSE)

/*
 * Returns 0 if entry exists in the database, 1 if it doesn't, and -1 on error.
 */
int
clippor_database_entry_exists(
    ClipporDatabase *self, ClipporEntry *entry, GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Id FROM Entries WHERE Id = ?";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(-1);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
        ret = 0;
    else if (ret == SQLITE_DONE)
        ret = 1;
    else
        STEP_ERROR(-1);

    sqlite3_finalize(stmt);
    return ret;
}

static char *
clippor_database_ref_data(ClipporDatabase *self, GBytes *bytes, GError **error)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(bytes != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement =
        "INSERT INTO Data (Data_id) "
        "VALUES (?) ON CONFLICT DO UPDATE SET Ref_count = Ref_count + 1;;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(NULL);

    char *data_id = g_compute_checksum_for_bytes(G_CHECKSUM_SHA1, bytes);

    sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret != SQLITE_DONE)
    {
        g_free(data_id);
        STEP_ERROR(NULL);
    }
    sqlite3_finalize(stmt);

    if (self->flags & CLIPPOR_DATABASE_IN_MEMORY)
        g_hash_table_insert(self->store, g_strdup(data_id), g_bytes_ref(bytes));
    else
    {
        size_t sz;
        const char *stuff = g_bytes_get_data(bytes, &sz);
        g_autofree char *path =
            g_strdup_printf("%s/data/%s", self->location_dir, data_id);

        if (!g_file_set_contents_full(
                path, stuff, sz, G_FILE_SET_CONTENTS_CONSISTENT, 0644, error
            ))
        {
            g_prefix_error(error, "Failed creating data file '%s': ", data_id);
            g_free(data_id);
            return NULL;
        }
    }

    return data_id;
}

/*
 * Unreferences data id by one, if it reaches zero, then it removes the row and
 * the associated data file from the filesystem.
 */
static gboolean
clippor_database_unref_data(
    ClipporDatabase *self, const char *data_id, GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(data_id != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "UPDATE Data"
                            "SET Ref_count = Ref_count - 1"
                            "WHERE Data_id = ? RETURNING Ref_count;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        int ref_count = sqlite3_column_int(stmt, 0);

        sqlite3_finalize(stmt);

        if (ref_count <= 0)
        {
            // Delete row and remove data file
            if (self->flags & CLIPPOR_DATABASE_IN_MEMORY)
                g_hash_table_remove(self->store, data_id);
            else
            {
                g_autofree char *path =
                    g_strdup_printf("%s/data/%s", self->location_dir, data_id);

                g_unlink(path);
            }

            statement = "DELETE From Data WHERE Data_id = ?;";

            PREPARE(FALSE);

            sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

            STEP_NO_ROW(FALSE);
        }

        return TRUE;
    }
    else
        STEP_ERROR(FALSE);
}

/*
 * Removes mime type rows in the database with id that doesn't exist in the hash
 * table.
 */
static gboolean
clippor_database_cleanup_mime_types(
    ClipporDatabase *self, const char *id, GHashTable *mime_types,
    GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(id != NULL);
    g_assert(mime_types != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement =
        "SELECT Mime_type,Data_id FROM Mime_types WHERE Id = ?;";
    const char *statement2 =
        "DELETE FROM Mime_types WHERE Id = ? AND Mime_type = ?;";
    sqlite3_stmt *stmt, *stmt2;
    ;
    int ret;

    PREPARE(FALSE);
    PREPARE2(FALSE);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    while ((ret = sqlite3_step(stmt)) == SQLITE_OK)
    {
        const char *mime_type = (const char *)sqlite3_column_text(stmt, 0);
        const char *data_id = (const char *)sqlite3_column_text(stmt, 1);

        if (!g_hash_table_contains(mime_types, mime_type))
        {
            // Delete mime type row first to avoid foriegn key restriction
            sqlite3_bind_text(stmt2, 1, id, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt2, 2, mime_type, -1, SQLITE_STATIC);

            ret = sqlite3_step(stmt);

            sqlite3_clear_bindings(stmt2);
            sqlite3_reset(stmt2);

            if (!clippor_database_unref_data(self, data_id, error))
            {
                sqlite3_finalize(stmt);
                sqlite3_finalize(stmt2);

                g_prefix_error(
                    error, "Failed cleaning up id '%s' in database: ", id
                );
                return FALSE;
            }
        }
    }

    sqlite3_finalize(stmt2);

    if (ret == SQLITE_ERROR)
        STEP_ERROR(FALSE);

    // STEP_ERROR finalizes stmt
    sqlite3_finalize(stmt);

    return TRUE;
}

/*
 * Given a hash table of mime types and the associated data, for each mime type,
 * create a new row with id in the Mime_types table, and for every piece of
 * data, create a new row in the Data table, or increase the reference count if
 * it already exists,
 *
 * If mime type already exists in the table, update it, or if
 * it exists in the database but not in the hash table, remove it.
 */
static gboolean
clippor_database_serialize_mime_types(
    ClipporDatabase *self, const char *id, GHashTable *mime_types,
    GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(id != NULL);
    g_assert(mime_types != NULL);
    g_assert(error == NULL || *error == NULL);

    g_autofree char *data_dir_path =
        g_strdup_printf("%s/data", self->location_dir);

    if (!(self->flags & CLIPPOR_DATABASE_IN_MEMORY) &&
        g_mkdir_with_parents(data_dir_path, 0755) == -1)
    {
        g_set_error(
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_DATA_DIR,
            "Failed creating directory '%s'", data_dir_path
        );
        return FALSE;
    }

    // Remove deleted mime types first
    if (!clippor_database_cleanup_mime_types(self, id, mime_types, error))
    {
        g_prefix_error(error, "Failed serializing mime types: ");
        return FALSE;
    }

    const char *statement =
        "INSERT INTO Mime_Types (Id, Mime_type, Data_id) "
        "VALUES (?, ?, ?) ON CONFLICT DO UPDATE SET Data_id = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    GHashTableIter iter;
    const char *mime_type;
    GBytes *bytes;

    g_hash_table_iter_init(&iter, mime_types);

    while (g_hash_table_iter_next(&iter, (void **)&mime_type, (void **)&bytes))
    {
        g_autofree char *data_id =
            clippor_database_ref_data(self, bytes, error);

        if (data_id == NULL)
        {
            g_prefix_error(
                error, "Failed serializing entry with id '%s': ", id
            );
            sqlite3_finalize(stmt);
            return FALSE;
        }

        sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, data_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, data_id, -1, SQLITE_STATIC);

        ret = sqlite3_step(stmt);

        if (ret != SQLITE_DONE)
            STEP_ERROR(FALSE);

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    }

    sqlite3_finalize(stmt);

    return TRUE;
}

/*
 * Serialize an entry into the database. If the entry already exists, it is
 * updated. The UPSERT clause is used so foreign key restrictions won't be
 * violated.
 */
gboolean
clippor_database_serialize_entry(
    ClipporDatabase *self, ClipporEntry *entry, GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    const char *statement = "BEGIN TRANSACTION;";
    char *err_msg;
    sqlite3_stmt *stmt = NULL;
    int ret;

    EXEC(FALSE);

    statement = "INSERT INTO Entries"
                "(Id, Creation_time, Last_used_time, Starred, Clipboard)"
                "VALUES (?, ?, ?, ?, ?)"
                "ON CONFLICT DO UPDATE SET "
                "Creation_time = ?, Last_used_time = ?, Starred = ?;";

    ret = sqlite3_prepare_v2(self->handle, statement, -1, &stmt, 0);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_PREPARE,
            "Failed preparing statement '%s': %s", statement,
            sqlite3_errmsg(self->handle)
        );
        goto fail;
    }

    const char *cb_label = clippor_entry_get_clipboard(entry);

    const char *id = clippor_entry_get_id(entry);
    int64_t creation_time = clippor_entry_get_creation_time(entry);
    int64_t last_used_time = clippor_entry_get_last_used_time(entry);
    gboolean starred = clippor_entry_is_starred(entry);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, creation_time);
    sqlite3_bind_int64(stmt, 3, last_used_time);
    sqlite3_bind_int(stmt, 4, starred);
    sqlite3_bind_text(stmt, 5, cb_label, -1, SQLITE_STATIC);

    sqlite3_bind_int64(stmt, 6, creation_time);
    sqlite3_bind_int64(stmt, 7, last_used_time);
    sqlite3_bind_int(stmt, 8, starred);

    ret = sqlite3_step(stmt);

    sqlite3_finalize(stmt);

    if (ret != SQLITE_DONE)
    {
        g_set_error(
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_PREPARE,
            "Failed stepping statement '%s': %s", statement,
            sqlite3_errmsg(self->handle)
        );
        goto fail;
    }

    if (!clippor_database_serialize_mime_types(
            self, id, clippor_entry_get_mime_types(entry), error
        ))
    {
        g_prefix_error_literal(error, "Failed serializing entry: ");
        goto fail;
    }

    gboolean f_ret = TRUE;

    if (FALSE)
fail:
        f_ret = FALSE;

    if (f_ret)
        statement = "COMMIT;";
    else
        statement = "ROLLBACK TRANSACTION;";

    EXEC(FALSE);

    return f_ret;
}

static gboolean
clippor_database_load_mime_types(
    ClipporDatabase *self, ClipporEntry *entry, GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Mime_type, Data_id FROM Mime_types "
                            "WHERE Id = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, clippor_entry_get_id(entry), -1, SQLITE_STATIC);

    // Temporarily store data with their data_id, so we can avoid loading the
    // same data file again creating duplicate GBytes.
    g_autoptr(GHashTable) store = g_hash_table_new_full(
        g_str_hash, g_str_equal, NULL, (GDestroyNotify)g_bytes_unref
    );

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char *mime_type = (const char *)sqlite3_column_text(stmt, 0);
        const char *data_id = (const char *)sqlite3_column_text(stmt, 1);

        // Check if we already loaded the same data before
        if (g_hash_table_contains(store, data_id))
        {
            clippor_entry_add_mime_type(
                entry, mime_type, g_hash_table_lookup(store, data_id)
            );
            continue;
        }

        g_autofree char *path =
            g_strdup_printf("%s/data/%s", self->location_dir, data_id);
        size_t sz;
        char *contents;

        if (!g_file_get_contents(path, &contents, &sz, error))
        {
            g_prefix_error(error, "Failed loading file '%s'", path);
            sqlite3_finalize(stmt);
            return FALSE;
        }

        GBytes *bytes = g_bytes_new_take(contents, sz);

        clippor_entry_add_mime_type(entry, mime_type, bytes);
        g_hash_table_insert(store, (char *)data_id, bytes);
    }

    sqlite3_finalize(stmt);

    return TRUE;
}

static ClipporEntry *
clippor_database_load_entry(
    ClipporDatabase *self, sqlite3_stmt *stmt, GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(stmt != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    int64_t creation_time = sqlite3_column_int64(stmt, 1);
    int64_t last_used_time = sqlite3_column_int64(stmt, 2);
    gboolean starred = sqlite3_column_int(stmt, 3);
    const char *cb = (const char *)sqlite3_column_text(stmt, 4);

    ClipporEntry *entry =
        clippor_entry_new_full(cb, id, creation_time, last_used_time, starred);

    if (!clippor_database_load_mime_types(self, entry, error))
    {
        g_object_unref(entry);
        return NULL;
    }

    return entry;
}

/*
 * Deserialize entry from database at given index that is associated with
 * given clipboard label.
 */
ClipporEntry *
clippor_database_deserialize_entry_at_index(
    ClipporDatabase *self, const char *cb, int64_t index, GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(index >= 0);
    g_assert(error == NULL || *error == NULL);

    const char *statement =
        "SELECT Id, Creation_time, Last_used_time, Starred, Clipboard "
        "FROM Entries WHERE Clipboard = ? "
        "ORDER BY Position DESC LIMIT 1 OFFSET ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(NULL);

    sqlite3_bind_text(stmt, 1, cb, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, index);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        ClipporEntry *entry = clippor_database_load_entry(self, stmt, error);

        sqlite3_finalize(stmt);

        if (entry == NULL)
            g_prefix_error(
                error, "Failed loading entry at index %ld for clipboard '%s'",
                index, cb
            );

        return entry;
    }
    else if (ret == SQLITE_DONE)
    {
        g_set_error(
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_ROW_NOT_EXIST,
            "No entry exists at index %ld", index
        );
    }
    else
        STEP_ERROR(NULL);

    sqlite3_finalize(stmt);

    return NULL;
}

/*
 * Deserialize entry from database with matching id.
 */
ClipporEntry *
clippor_database_deserialize_entry_with_id(
    ClipporDatabase *self, const char *id, GError **error
)
{
    g_assert(CLIPPOR_IS_DATABASE(self));
    g_assert(id != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement =
        "SELECT Id, Creation_time, Last_used_time, Starred, Clipboard "
        "FROM Entries WHERE Id = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(NULL);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        ClipporEntry *entry = clippor_database_load_entry(self, stmt, error);

        sqlite3_finalize(stmt);

        if (entry == NULL)
            g_prefix_error(error, "Failed loading entry with id '%s': ", id);

        return entry;
    }
    else if (ret == SQLITE_DONE)
    {
        g_set_error(
            error, CLIPPOR_DATABASE_ERROR, CLIPPOR_DATABASE_ERROR_ROW_NOT_EXIST,
            "No entry exists with id '%s'", id
        );
    }
    else
        STEP_ERROR(NULL);

    sqlite3_finalize(stmt);

    return NULL;
}
