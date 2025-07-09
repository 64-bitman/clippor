#include "database.h"
#include "clippor-entry.h"
#include "util.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <inttypes.h>
#include <sqlite3.h>

G_DEFINE_QUARK(database_error_quark, database_error)

static sqlite3 *DB = NULL;
static char *STORE_DIR = NULL, *DATA_DIR = NULL;

#define EXEC_ERROR(ret)                                                        \
    do                                                                         \
    {                                                                          \
        g_set_error(                                                           \
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE_EXEC,                 \
            "Failed executing statement '%s': %s", statement, err_msg          \
        );                                                                     \
        sqlite3_free(err_msg);                                                 \
        return FALSE;                                                          \
    } while (FALSE)

#define PREPARE_ERROR(ret)                                                     \
    do                                                                         \
    {                                                                          \
        g_set_error(                                                           \
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE_PREPARE,              \
            "Failed preparing statement '%s': %s", statement,                  \
            sqlite3_errmsg(DB)                                                 \
        );                                                                     \
        return ret;                                                            \
    } while (FALSE)

#define STEP_ERROR(ret)                                                        \
    do                                                                         \
    {                                                                          \
        g_set_error(                                                           \
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE_STEP,                 \
            "Failed stepping statement '%s': %s:", statement,                  \
            sqlite3_errmsg(DB)                                                 \
        );                                                                     \
        sqlite3_finalize(stmt);                                                \
        return FALSE;                                                          \
    } while (FALSE)

#define PREPARE(error_ret)                                                     \
    do                                                                         \
    {                                                                          \
        ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);              \
        if (ret != SQLITE_OK)                                                  \
            PREPARE_ERROR(FALSE);                                              \
    } while (FALSE)

#define STEP_SINGLE(error_ret)                                                 \
    do                                                                         \
    {                                                                          \
        ret = sqlite3_step(stmt);                                              \
        if (ret != SQLITE_DONE)                                                \
            STEP_ERROR(FALSE);                                                 \
        sqlite3_finalize(stmt);                                                \
    } while (FALSE)

/*
 * Check if the database is outdated and update it. Create the version table if
 * it does not exist.
 */
static gboolean
update_database_version(GError **error)
{
    g_assert(error == NULL || *error == NULL);

    int ret;
    char *err_msg;

    const char *statement = "CREATE TABLE IF NOT EXISTS Version("
                            "Db_version INTEGER PRIMARY KEY);"
                            "INSERT OR IGNORE INTO Version "
                            "VALUES (" STRING(DATABASE_VERSION) ");";

    ret = sqlite3_exec(DB, statement, NULL, NULL, &err_msg);

    if (ret != SQLITE_OK)
        EXEC_ERROR(FALSE);

    statement = "SELECT Db_version FROM Version;";
    sqlite3_stmt *stmt;
    ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);
    int db_version;

    if (ret != SQLITE_OK)
        PREPARE_ERROR(FALSE);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
        db_version = sqlite3_column_int(stmt, 0);
    else
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);

    if (db_version < DATABASE_VERSION)
    {
        // Add code when we update database version if we do
        g_info("Updating database from %d to %d", db_version, DATABASE_VERSION);
    }

    statement =
        "INSERT OR REPLACE INTO Version VALUES (" STRING(DATABASE_VERSION) ");";

    return TRUE;
}

gboolean
database_init(GError **error)
{
    g_assert(error == NULL || *error == NULL);

    const char *data_dir = g_get_user_data_dir();
    STORE_DIR = g_strdup_printf("%s/clippor", data_dir);
    DATA_DIR = g_strdup_printf("%s/data", STORE_DIR);

    if (g_mkdir_with_parents(STORE_DIR, 0755) == -1)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_NO_DATA_DIR,
            "Failed creating database directory: %s", g_strerror(errno)
        );
        g_free(STORE_DIR);
        return FALSE;
    }

    char *db_path = g_strdup_printf("%s/history.sqlite3", STORE_DIR);

    int ret = sqlite3_open(db_path, &DB);
    const char *statement;
    char *err_msg;

    g_free(db_path);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE_OPEN,
            "sqlite3_open() failed: %s", sqlite3_errmsg(DB)
        );
        sqlite3_close(DB);

        return FALSE;
    }

    // Create tables
    statement =
        "PRAGMA foreign_keys = ON;"
        "PRAGMA synchronous = NORMAL;"
        "PRAGMA journal_mode = WAL;"
        "CREATE TABLE IF NOT EXISTS Entries("
        "Position INTEGER PRIMARY KEY AUTOINCREMENT,"
        "Id char(40) NOT NULL UNIQUE,"
        "Creation_time INTEGER NOT NULL CHECK (Creation_time > 0),"
        "Last_used_time INTEGER NOT NULL CHECK (Last_used_time > 0),"
        "Starred BOOLEAN DEFAULT false,"
        "Clipboard TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS Mime_types("
        "Id char(40) NOT NULL,"
        "Mime_type TEXT NOT NULL,"
        "Data_id char(40) NOT NULL,"
        "PRIMARY KEY (Id, Mime_type),"
        "FOREIGN KEY (Id) REFERENCES Entries(Id) ON DELETE RESTRICT,"
        "FOREIGN KEY (Data_id) REFERENCES Data(Data_id) ON DELETE RESTRICT"
        ");"
        "CREATE TABLE IF NOT EXISTS Data("
        "Data_id char(40) PRIMARY KEY,"
        "Ref_count INTEGER NOT NULL CHECK (Ref_count >= 0)"
        ");";

    ret = sqlite3_exec(DB, statement, NULL, NULL, &err_msg);

    if (ret != SQLITE_OK)
    {
        sqlite3_close(DB);
        EXEC_ERROR(FALSE);
    }

    if (!update_database_version(error))
    {
        sqlite3_close(DB);
        return FALSE;
    }

    return TRUE;
}

void
database_uninit(void)
{
    g_free(STORE_DIR);
    g_free(DATA_DIR);
    sqlite3_close(DB);
}

/*
 * Create a new row for entry in the database
 */
gboolean
database_new_entry_row(ClipporEntry *entry, GError **error)
{
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    const char *statement =
        "INSERT INTO Entries "
        "(Id, Creation_time, Last_used_time, Starred, Clipboard)"
        "VALUES (?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, clippor_entry_get_id(entry), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, clippor_entry_get_creation_time(entry));
    sqlite3_bind_int64(stmt, 3, clippor_entry_get_last_used_time(entry));
    sqlite3_bind_int(stmt, 4, clippor_entry_is_starred(entry));
    sqlite3_bind_text(
        stmt, 5,
        clippor_clipboard_get_label(clippor_entry_get_clipboard(entry)), -1,
        SQLITE_STATIC
    );

    STEP_SINGLE(FALSE);

    return TRUE;
}

/*
 * Create a new row for data in the 'Data' table if it doesn't exist and write
 * the data to a file. If it does exist, increase the reference count. Returns
 * the calculated Data_id in an allocated string.
 */
static const char *
database_ref_data_row(ClipporData *data, GError **error)
{
    g_assert(data != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "INSERT OR IGNORE INTO Data"
                            "(Data_id, Ref_count)"
                            "VALUES (?, 0)";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(NULL);

    const char *data_id = clippor_data_get_checksum(data);

    sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

    STEP_SINGLE(FALSE);

    // If row was created, Ref_count will be increased to one, else +1.
    statement = "UPDATE Data "
                "SET Ref_count = Ref_count + 1 "
                "WHERE Data_id = ?;";

    PREPARE(NULL);

    sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

    STEP_SINGLE(NULL);

    if (g_mkdir_with_parents(DATA_DIR, 0755) == -1)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ADD,
            "Failed creating directory '%s': %s", DATA_DIR, g_strerror(errno)
        );
        return NULL;
    }

    // Attempt to write to a file it it doesn't exist
    g_autofree char *path = g_strdup_printf("%s/%s", DATA_DIR, data_id);

    if (g_access(path, F_OK) == 0)
        return data_id;

    size_t sz;
    const char *d = clippor_data_get_data(data, &sz);

    if (!g_file_set_contents_full(
            path, d, sz, G_FILE_SET_CONTENTS_CONSISTENT, 0644, error
        ))
    {
        GError *old_err = *error;
        *error = NULL;

        // Remove rows
        statement = "DELETE FROM Data WHERE Data_id = ?;";

        PREPARE(NULL);

        sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

        STEP_SINGLE(NULL);

        *error = old_err;
        return NULL;
    }

    return data_id;
}

/*
 * Create a new row for mime type associated with the given entry, and
 * creates a file if it doesn't exist to store the data. This will also create a
 * new row in the 'Data' table to track the passed data
 */
gboolean
database_new_mime_type_row(
    ClipporEntry *entry, const char *mime_type, ClipporData *data,
    GError **error
)
{
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(mime_type != NULL);
    g_assert(error == NULL || *error == NULL);

    if (data == NULL)
        return TRUE;

    const char *data_id = database_ref_data_row(data, error);

    if (data_id == NULL)
    {
        g_prefix_error_literal(error, "Failed creating new mime type row: ");
        return FALSE;
    }

    const char *statement = "INSERT INTO Mime_types"
                            "(Id, Mime_type, Data_id)"
                            "VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, clippor_entry_get_id(entry), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, data_id, -1, SQLITE_STATIC);

    STEP_SINGLE(FALSE);

    return TRUE;
}

/*
 * Returns an array of mime types associated with the given id. If there are
 * none, returns an empty array.
 */
static GPtrArray *
database_get_mime_types_with_id(const char *id, GError **error)
{
    g_assert(id != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Mime_type FROM Mime_types WHERE Id = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(NULL);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    GPtrArray *arr = g_ptr_array_new_full(10, g_free);

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char *s = (const char *)sqlite3_column_text(stmt, 0);

        g_ptr_array_add(arr, g_strdup(s));
    }

    if (ret != SQLITE_DONE)
    {
        g_ptr_array_unref(arr);
        STEP_ERROR(NULL);
    }

    return arr;
}

/*
 * Creates an entry object from an SQlite row in the 'Entries' table.
 */
static ClipporEntry *
database_create_entry_from_row(
    ClipporClipboard *cb, sqlite3_stmt *stmt, GError **error
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(stmt != NULL);

    const char *id = (const char *)sqlite3_column_text(stmt, 0);
    int64_t creation_time = sqlite3_column_int64(stmt, 1);
    int64_t last_used_time = sqlite3_column_int64(stmt, 2);
    gboolean starred = sqlite3_column_int(stmt, 3);

    GPtrArray *mime_types = database_get_mime_types_with_id(id, error);

    if (mime_types == NULL)
    {
        sqlite3_finalize(stmt);
        return NULL;
    }

    ClipporEntry *entry = clippor_entry_new_no_database(
        NULL, creation_time, id, cb, CLIPPOR_SELECTION_TYPE_NONE
    );

    g_object_set(
        entry, "starred", starred, "last-used-time", last_used_time, NULL
    );

    // Add mime types
    for (uint i = 0; i < mime_types->len; i++)
        clippor_entry_set_mime_type_no_database(
            entry, mime_types->pdata[i], NULL
        );

    g_ptr_array_unref(mime_types);

    sqlite3_finalize(stmt);

    return entry;
}

/*
 * Get entry by its position, returning NULL if it doesn't exist
 */
ClipporEntry *
database_get_entry_by_position(
    ClipporClipboard *cb, int64_t index, GError **error
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(index >= 0);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Id, Creation_time, Last_used_time, Starred "
                            "FROM Entries WHERE Clipboard = ? "
                            "ORDER BY Position DESC LIMIT 1 OFFSET ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(NULL);

    sqlite3_bind_text(
        stmt, 1, clippor_clipboard_get_label(cb), -1, SQLITE_STATIC
    );
    sqlite3_bind_int64(stmt, 2, index);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
        return database_create_entry_from_row(cb, stmt, error);
    else if (ret == SQLITE_DONE)
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "No row exists at index %" PRIu64, index
        );
    else
        STEP_ERROR(NULL);

    sqlite3_finalize(stmt);

    return NULL;
}

ClipporEntry *
database_get_entry_by_id(ClipporClipboard *cb, const char *id, GError **error)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(id != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Creation_time, Last_used_time, Starred "
                            "FROM Entries WHERE Id = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(NULL);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
        return database_create_entry_from_row(cb, stmt, error);
    else if (ret == SQLITE_DONE)
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "No row exists with id '%s'", id
        );
    else
        STEP_ERROR(NULL);

    sqlite3_finalize(stmt);

    return NULL;
}

/*
 * Gets data associated with mime type for given entry. Returns NULL if there is
 * no data, or if there is an error.
 */
ClipporData *
database_get_entry_mime_type_data(
    ClipporEntry *entry, const char *mime_type, GError **error
)
{
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(mime_type != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Data_id FROM Mime_types "
                            "WHERE Id = ? AND Mime_type = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(NULL);

    sqlite3_bind_text(stmt, 1, clippor_entry_get_id(entry), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {

        const char *data_id = (const char *)sqlite3_column_text(stmt, 0);
        g_autofree char *path = g_strdup_printf("%s/%s", DATA_DIR, data_id);

        sqlite3_finalize(stmt);

        // Read from data file
        char *data;
        size_t sz;

        if (!g_file_get_contents(path, &data, &sz, error))
        {
            g_prefix_error(error, "Failed reading file '%s': ", path);
            return NULL;
        }

        return clippor_data_new_take(data, sz, TRUE);
    }
    else if (ret != SQLITE_DONE)
        STEP_ERROR(NULL);

    // Mime type doesnt exist
    sqlite3_finalize(stmt);

    return NULL;
}

/*
 * Unreferences the ref_count for data_id in the 'Data' table by one. If it
 * reaches zero, then also remove the file which contains the data and delete
 * the row.
 */
static gboolean
database_unref_data_row(const char *data_id, GError **error)
{
    g_assert(data_id != NULL);
    g_assert(error == NULL || *error == NULL);

    // Unreference by one
    const char *statement = "UPDATE Data "
                            "SET Ref_count = Ref_count - 1 "
                            "WHERE Data_id = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

    STEP_SINGLE(FALSE);

    // Check if ref_count reached zero, if so delete mapped file and row
    statement = "SElECT Ref_count FROM Data WHERE Data_id = ?;";

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        int64_t ref_count = sqlite3_column_int64(stmt, 0);

        sqlite3_finalize(stmt);

        if (ref_count == 0)
        {
            // Delete file
            g_autofree char *path = g_strdup_printf("%s/%s", DATA_DIR, data_id);

            g_remove(path);

            // Delete row
            statement = "DELETE FROM Data WHERE Data_id = ?;";

            PREPARE(FALSE);

            sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

            STEP_SINGLE(FALSE);
        }

        return TRUE;
    }
    else
        STEP_ERROR(FALSE);

    return TRUE;
}

/*
 * Removes mime type with given id from database, and unreferences the data it
 * maps to.
 */
gboolean
database_remove_mime_type_row_by_id(
    const char *id, const char *mime_type, GError **error
)
{
    g_assert(id != NULL);
    g_assert(mime_type != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Data_id FROM Mime_types "
                            "WHERE Id = ? AND Mime_type = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        g_autofree char *data_id =
            g_strdup((const char *)sqlite3_column_text(stmt, 0));

        sqlite3_finalize(stmt);

        statement = "DELETE FROM Mime_Types WHERE Id = ? AND Mime_type = ?;";

        PREPARE(FALSE);

        sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);

        STEP_SINGLE(FALSE);

        // Must do this last in case data row is deleted, so that we won't get
        // affected by the foreign key constraint.
        return database_unref_data_row(data_id, error);
    }
    else if (ret == SQLITE_DONE)
    {
        // Mime type doesnt exist
        sqlite3_finalize(stmt);
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "Mime type '%s' with id '%s' does not exist in the database",
            mime_type, id
        );
        return FALSE;
    }
    else
        STEP_ERROR(FALSE);
}

/*
 * Removes row in 'Entries' table with matching id, as well as removing its mime
 * type rows and possibly the files they map to.
 */
gboolean
database_remove_entry_row_by_id(const char *id, GError **error)
{
    g_assert(id != NULL);
    g_assert(error == NULL || *error == NULL);

    // First delete mime types since they have a foreign key dependency on the
    // 'Entries' row.
    const char *statement = "SELECT Mime_type FROM Mime_types WHERE Id = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char *mime_type = (const char *)sqlite3_column_text(stmt, 0);

        if (!database_remove_mime_type_row_by_id(id, mime_type, error))
        {
            sqlite3_finalize(stmt);
            g_prefix_error(
                error,
                "Failed removing entry row with id '%s' from database: ", id
            );
            return FALSE;
        }
    }

    if (ret != SQLITE_DONE)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);

    // Delete row
    statement = "DELETE FROM Entries WHERE Id = ?;";

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    STEP_SINGLE(FALSE);

    return TRUE;
}

/*
 * Trim entries and their data from database according to max-entries for
 * clipboard. If "all" is TRUE then remove all rows associated with clipboard
 */
gboolean
database_trim_entry_rows(ClipporClipboard *cb, gboolean all, GError **error)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(error == NULL || *error == NULL);

    int64_t max_entries = clippor_clipboard_get_max_entries(cb);

    const char *statement;

    if (all)
        statement = "SELECT Id FROM Entries WHERE Clipboard = ?";
    else
        statement = "SELECT Id FROM Entries "
                    "WHERE Position NOT IN ("
                    "SELECT Position FROM Entries "
                    "Where Clipboard = ? "
                    "ORDER BY Position DESC "
                    "LIMIT ?"
                    ");";

    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(
        stmt, 1, clippor_clipboard_get_label(cb), -1, SQLITE_STATIC
    );
    if (!all)
        sqlite3_bind_int64(stmt, 2, max_entries);

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);

        if (!database_remove_entry_row_by_id(id, error))
        {
            g_prefix_error(
                error, "Failed trimming entries for clipboard '%s': ",
                clippor_clipboard_get_label(cb)
            );
            sqlite3_finalize(stmt);
            return FALSE;
        }
    }

    if (ret != SQLITE_DONE)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);
    return TRUE;
}

/*
 * Updates mime type row with new data. Difference from just using INSERT OR
 * REPLACE is that it also decrements the refernce count on the data row
 * with the old data_id. If "data" is NULL, delete the mime type row instead of
 * updating it.
 */
gboolean
database_update_mime_type_row(
    ClipporEntry *entry, const char *mime_type, ClipporData *data,
    GError **error
)
{
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(mime_type != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *id = clippor_entry_get_id(entry);

    if (data == NULL)
        // Delete row
        return database_remove_mime_type_row_by_id(id, mime_type, error);

    // Unreference old mime type data
    const char *statement = "SELECT Data_id FROM Mime_types "
                            "WHERE Id = ? AND Mime_type = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        const char *data_id = (const char *)sqlite3_column_text(stmt, 0);

        if (!database_unref_data_row(data_id, error))
        {
            g_prefix_error_literal(error, "Failed updating mime type row: ");
            sqlite3_finalize(stmt);
            return FALSE;
        }
        sqlite3_finalize(stmt);
    }
    else if (ret == SQLITE_DONE)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "Mime type '%s' does not exist in the database", mime_type
        );
        sqlite3_finalize(stmt);
        return FALSE;
    }
    else
        STEP_ERROR(FALSE);

    const char *data_id = database_ref_data_row(data, error);

    if (data_id == NULL)
    {
        g_prefix_error_literal(error, "Failed updating mime type row: ");
        return FALSE;
    }

    // Update row
    statement = "UPDATE Mime_types "
                "SET Data_id = ? "
                "WHERE Id = ? AND Mime_type = ?;";

    PREPARE(FALSE);

    sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, clippor_entry_get_id(entry), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, mime_type, -1, SQLITE_STATIC);

    STEP_SINGLE(FALSE);

    return TRUE;
}

/*
 * Get number of entries for clipboard in database. Returns -1 on error
 */
int64_t
database_get_num_entries(ClipporClipboard *cb, GError **error)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT COUNT(*) FROM Main WHERE Clipboard = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(-1);

    sqlite3_bind_text(
        stmt, 1, clippor_clipboard_get_label(cb), -1, SQLITE_STATIC
    );

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        int64_t num = sqlite3_column_int64(stmt, 0);

        sqlite3_finalize(stmt);
        return num;
    }
    else if (ret == SQLITE_DONE)
    {
        g_set_error_literal(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE_STEP,
            "Could not get count of database"
        );
        sqlite3_finalize(stmt);
        return -1;
    }
    else
        STEP_ERROR(-1);
}

/*
 * Returns 0 if entry exist, 1 if entry doesn't exist, and -1 on error.
 */
int
database_entry_id_exists(const char *id, GError **error)
{
    g_assert(id != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Id FROM Main WHERE Id = ?;";
    sqlite3_stmt *stmt;
    int ret;

    PREPARE(-1);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        sqlite3_finalize(stmt);
        return 0;
    }
    else if (ret == SQLITE_DONE)
    {
        sqlite3_finalize(stmt);
        return 1;
    }
    else
        STEP_ERROR(-1);
}
