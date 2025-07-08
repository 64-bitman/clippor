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

    // Setup main table where the history is stored
    //
    // Don't see much value in foreign keys for mapping Main id to Map id,
    // requires an extra loop through mime types when adding an entry + a
    // discrepency is not a big deal.
    statement = "CREATE TABLE IF NOT EXISTS Main ("
                "Position INTEGER PRIMARY KEY AUTOINCREMENT,"
                "Id char(40) NOT NULL,"
                "Mime_types TEXT NOT NULL,"
                "Creation_time INTEGER NOT NULL CHECK (Creation_time > 0),"
                "Last_used_time INTEGER NOT NULL CHECK (Last_used_time > 0),"
                "Starred BOOLEAN,"
                "Clipboard TEXT NOT NULL"
                ");"
                "CREATE TABLE IF NOT EXISTS Map("
                "Id char(40) NOT NULL,"
                "Mime_type TEXT NOT NULL,"
                "Data_id char(40) NOT NULL" // Filename of data file
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
 * Add mime type to 'Map' table.
 */
static gboolean
database_add_mime_type(
    const char *id, const char *mime_type, const char *data_id, GError **error
)
{
    g_assert(id != NULL);
    g_assert(mime_type != NULL);
    g_assert(data_id != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "INSERT INTO Map VALUES (?, ?, ?);";
    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(FALSE);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, data_id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret != SQLITE_DONE)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);

    return TRUE;
}

/*
 * Write data to a persistent file. Returns the filename of the data file
 */
static char *
database_write_mime_type(const char *store_dir, GBytes *data, GError **error)
{
    g_assert(store_dir != NULL);
    g_assert(data != NULL);
    g_assert(error == NULL || *error == NULL);

    // Create filename using the checksum of the data
    char *filename = g_compute_checksum_for_bytes(G_CHECKSUM_SHA1, data);
    g_autofree char *filepath = g_strdup_printf("%s/%s", store_dir, filename);

    // Ignore if file already exists, return checksum
    if (g_access(filepath, F_OK) == 0)
        return filename;

    size_t sz;
    const char *d = g_bytes_get_data(data, &sz);

    if (!g_file_set_contents_full(
            filepath, d, sz, G_FILE_SET_CONTENTS_CONSISTENT, 0644, error
        ))
    {
        g_free(filename);
        return NULL;
    }

    return filename;
}

/*
 * Creates a new entry in the database or replaces an existing one and writes
 * data to a persistent location
 */
gboolean
database_set_entry(ClipporEntry *entry, GError **error)
{
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    if (DB == NULL)
        return TRUE;

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);
    int64_t creation_time = clippor_entry_get_creation_time(entry);
    int64_t last_used_time = clippor_entry_get_last_used_time(entry);
    const char *id = clippor_entry_get_id(entry);
    const char *cb_label =
        clippor_clipboard_get_label(clippor_entry_get_clipboard(entry));

    // Id is used for the directory name to store all data for this entry
    if (g_mkdir_with_parents(DATA_DIR, 0755) == -1)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ADD,
            "Failed creating directory '%s': %s", DATA_DIR, g_strerror(errno)
        );
        return FALSE;
    }

    GHashTableIter iter;
    char *mime_type;
    GBytes *data;
    g_autoptr(GString) mime_list = g_string_new(NULL);

    g_hash_table_iter_init(&iter, mime_types);

    while (
        g_hash_table_iter_next(&iter, (gpointer *)&mime_type, (gpointer *)&data)
    )
    {
        GError *error = NULL;

        g_autofree char *data_id =
            database_write_mime_type(DATA_DIR, data, &error);

        if (data_id == NULL)
        {
            g_assert(error != NULL);

            g_message(
                "Failed writing mime type '%s': %s", mime_type, error->message
            );
            g_error_free(error);
            continue;
        }
        g_assert(error == NULL);

        // Add mime type to database file
        if (!database_add_mime_type(id, mime_type, data_id, &error))
        {
            g_assert(error != NULL);

            g_message(
                "Failed adding mime type '%s' to database: %s", mime_type,
                error->message
            );
            g_error_free(error);
            continue;
        }
        g_string_append_printf(mime_list, "%s,", mime_type);
    }
    g_string_truncate(mime_list, mime_list->len - 1); // Remove trailling comma

    // Add row for entry into database
    const char *statement = "INSERT OR REPLACE INTO Main "
                            "VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(FALSE);

    sqlite3_bind_null(stmt, 1);
    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, mime_list->str, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, creation_time);
    sqlite3_bind_int64(stmt, 5, last_used_time);
    sqlite3_bind_int(stmt, 6, clippor_entry_is_starred(entry));
    sqlite3_bind_text(stmt, 7, cb_label, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret != SQLITE_DONE)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);

    return TRUE;
}

/*
 * Derizalize entry at index in database to a ClipporEntry for clipboard. An
 * index of 0 indicates the most recent entry in the database. Note that users
 * may move entries around, so "recent" is not the best way to describe it.
 * If id is not NULL, then it is used to identify the entry instead of the
 * index.
 */
ClipporEntry *
database_deserialize_entry(
    ClipporClipboard *cb, int64_t index, const char *id, GError **error
)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(error == NULL || *error == NULL);

    // Get row in Main table
    const char *statement;

    if (id == NULL)
        statement = "SELECT Id, Mime_types, Creation_time,"
                    "Last_used_time, Starred FROM Main "
                    "WHERE Clipboard = ?"
                    "ORDER BY Position DESC "
                    "LIMIT 1 OFFSET ?";
    else
        statement = "SELECT Id, Mime_types, Creation_time,"
                    "Last_used_time, Starred FROM Main "
                    "WHERE Clipboard = ? AND Id = ?";

    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(NULL);

    sqlite3_bind_text(
        stmt, 1, clippor_clipboard_get_label(cb), -1, SQLITE_STATIC
    );
    if (id == NULL)
        sqlite3_bind_int64(stmt, 2, index);
    else
        sqlite3_bind_text(stmt, 2, id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *mime_list = (const char *)sqlite3_column_text(stmt, 1);
        int64_t creation_time = sqlite3_column_int64(stmt, 2);
        int64_t last_used_time = sqlite3_column_int64(stmt, 3);
        gboolean starred = sqlite3_column_int(stmt, 4);

        ClipporEntry *entry = clippor_entry_new(
            NULL, creation_time, id, cb, CLIPPOR_SELECTION_TYPE_NONE
        );

        g_object_set(
            entry, "starred", starred, "last-used-time", last_used_time, NULL
        );

        // Add mime types (without any data)
        gchar **mime_types = g_strsplit(mime_list, ",", -1);

        for (char **mime_type = mime_types; *mime_type != NULL; mime_type++)
            clippor_entry_set_mime_type(entry, *mime_type, NULL);

        g_strfreev(mime_types);

        sqlite3_finalize(stmt);
        return entry;
    }
    else if (ret == SQLITE_DONE)
    {
        // No such row exists
        if (id == NULL)
            g_set_error(
                error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
                "No row exists at index %" PRIu64, index
            );
        else
            g_set_error(
                error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
                "No row exists at with id '%s'", id
            );
    }
    else
        STEP_ERROR(NULL);

    // Shouldn't happen
    sqlite3_finalize(stmt);
    return NULL;
}

/*
 * Return data from the database with given entry and mime type
 */
GBytes *
database_deserialize_mime_type(
    ClipporEntry *entry, const char *mime_type, GError **error
)
{
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(mime_type != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Data_id FROM Map "
                            "WHERE Id = ? AND Mime_type = ?";
    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(NULL);

    sqlite3_bind_text(stmt, 1, clippor_entry_get_id(entry), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        const char *data_id = (const char *)sqlite3_column_text(stmt, 0);
        g_autofree char *filepath =
            g_strdup_printf("%s/data/%s", STORE_DIR, data_id);

        sqlite3_finalize(stmt);

        // Read from data file
        char *data;
        size_t sz;

        if (!g_file_get_contents(filepath, &data, &sz, error))
        {
            g_prefix_error(error, "Failed reading file '%s': ", filepath);
            return NULL;
        }

        return g_bytes_new_take(data, sz);
    }
    else if (ret == SQLITE_DONE)
        // No such row exists
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "Mime type %s with id %s does not exist in database", mime_type,
            clippor_entry_get_id(entry)
        );
    else
        STEP_ERROR(NULL);

    // Shouldn't happen
    sqlite3_finalize(stmt);
    return NULL;
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

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(-1);

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
        // Shouldn't happen?
        g_set_error_literal(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "Could not get count of database"
        );
    else
        STEP_ERROR(-1);

    // Shouldn't happen
    sqlite3_finalize(stmt);
    return -1;
}

/*
 * Returns index of entry in the database, starts at 0. Returns -1 on error
 */
int64_t
database_get_entry_index(ClipporEntry *entry, GError **error)
{
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Position FROM Main WHERE Id = ?;";
    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(-1);

    sqlite3_bind_text(stmt, 1, clippor_entry_get_id(entry), -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        int64_t num = sqlite3_column_int64(stmt, 0);

        sqlite3_finalize(stmt);
        return num - 1; // Position starts at 1
    }
    else if (ret == SQLITE_DONE)
        // No such row exists
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "Entry with id %s does not exist in database",
            clippor_entry_get_id(entry)
        );
    else
        STEP_ERROR(-1);

    // Shouldn't happen
    sqlite3_finalize(stmt);
    return -1;
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

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(-1);

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

    // Shouldn't happen
    sqlite3_finalize(stmt);
    return -1;
}

/*
 * Returns number of ids that reference a data_id, else -1 on error.
 */
static int
database_num_id_own_data_id(const char *data_id, GError **error)
{
    g_assert(data_id != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT COUNT(DISTINCT Id) FROM Map "
                            "WHERE Data_id = ?;";
    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(-1);

    sqlite3_bind_text(stmt, 1, data_id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        int num = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return num;
    }
    else
        STEP_ERROR(-1);

    // Shouldn't happen
    sqlite3_finalize(stmt);
    return -1;
}

/*
 * Remove rows with given id from 'Map' table in database, including the data
 * files they map to.
 */
static gboolean
database_remove_mime_types(const char *id, GError **error)
{
    g_assert(id != NULL);
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT Data_id FROM Map "
                            "WHERE Id = ?";
    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(FALSE);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        const char *data_id = (const char *)sqlite3_column_text(stmt, 0);
        int num = database_num_id_own_data_id(data_id, error);

        if (num == -1)
        {
            g_prefix_error_literal(error, "Failed removing mime type: ");
            sqlite3_finalize(stmt);
            return FALSE;
        }

        // Only remove file if no other ids are referencing it
        if (num <= 1)
        {
            char *path = g_strdup_printf("%s/%s", DATA_DIR, data_id);

            g_remove(path);
            g_free(path);
        }
    }

    if (ret != SQLITE_DONE)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);

    // Remove rows from table
    statement = "DELETE FROM Map "
                "WHERE Id = ?";
    ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(FALSE);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    ret = sqlite3_step(stmt);

    if (ret != SQLITE_DONE && ret != SQLITE_ROW)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);

    return TRUE;
}

/*
 * Trim entries and their data from database according to max-entries for
 * clipboard. If "all" is TRUE then remove all rows associated with clipboard
 */
gboolean
database_trim_entries(ClipporClipboard *cb, gboolean all, GError **error)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(error == NULL || *error == NULL);

    int64_t max_entries = clippor_clipboard_get_max_entries(cb);

    // TODO: implement code!!
    const char *statement;

    if (all)
        statement = "SELECT Id FROM Main WHERE Clipboard = ?";
    else
        statement = "SELECT Id FROM Main "
                    "WHERE Position NOT IN ("
                    "SELECT Position FROM Main "
                    "Where Clipboard = ? "
                    "ORDER BY Position DESC "
                    "LIMIT ?"
                    ");";

    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(FALSE);

    sqlite3_bind_text(
        stmt, 1, clippor_clipboard_get_label(cb), -1, SQLITE_STATIC
    );
    if (!all)
        sqlite3_bind_int64(stmt, 2, max_entries - 1);

    while ((ret = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        GError *error2 = NULL;
        const char *id = (const char *)sqlite3_column_text(stmt, 0);

        if (!database_remove_mime_types(id, &error2))
        {
            g_assert(error2 != NULL);
            g_warning("Failed removing mime type: %s", error2->message);
            g_error_free(error2);
            continue;
        }
    }

    if (ret != SQLITE_DONE)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);

    // Remove rows from table
    if (all)
        statement = "DELETE FROM Main WHERE Clipboard = ?";
    else
        statement = "DELETE FROM Main "
                    "WHERE Position NOT IN ("
                    "SELECT Position FROM Main "
                    "Where Clipboard = ? "
                    "ORDER BY Position DESC "
                    "LIMIT ?"
                    ");";
    ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(FALSE);

    sqlite3_bind_text(
        stmt, 1, clippor_clipboard_get_label(cb), -1, SQLITE_STATIC
    );
    if (!all)
        sqlite3_bind_int64(stmt, 2, max_entries - 1);

    ret = sqlite3_step(stmt);

    if (ret != SQLITE_DONE && ret != SQLITE_ROW)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);
    return TRUE;
}

/*
 * Remove row with id from 'Main' table and 'Map' table
 */
gboolean
database_remove_id(const char *id, GError **error)
{
    g_assert(id != NULL);
    g_assert(error == NULL || *error == NULL);

    int ret = database_entry_id_exists(id, error);

    if (ret == 1)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "Entry id '%s' does not exist", id
        );
        return FALSE;
    }
    else if (ret == -1)
    {
        g_assert(error == NULL || *error != NULL);
        g_prefix_error_literal(error, "Failed removing entry id: ");
        return FALSE;
    }

    const char *statement = "DELETE FROM Main "
                            "WHERE Id = ?;";
    sqlite3_stmt *stmt;

    ret = sqlite3_prepare_v2(DB, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
        PREPARE_ERROR(-1);

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret != SQLITE_DONE && ret != SQLITE_ROW)
        STEP_ERROR(FALSE);

    sqlite3_finalize(stmt);

    return database_remove_mime_types(id, error);
}
