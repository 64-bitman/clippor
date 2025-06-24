#include "database.h"
#include "clippor-entry.h"
#include "global.h"
#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>
#include <inttypes.h>
#include <sqlite3.h>

G_DEFINE_QUARK(database_error_quark, database_error)

static sqlite3 *db = NULL;
const char *store_dir = NULL;

/*
 * Remember to free returned value
 */
static char *
get_database_directory(void)
{
    // Try using $XDG_DATA_HOME first
    const char *xdgdatahome = g_getenv("XDG_DATA_HOME");
    char *path;

    if (xdgdatahome != NULL)
    {
        path = g_strdup_printf("%s/clippor", xdgdatahome);

        if (g_mkdir_with_parents(path, 0755) == 0)
            return path;

        g_free(path);
    }

    // Try ~/.local/share/clippor
    path = g_strdup_printf("%s/.local/share/clippor", g_getenv("HOME"));

    if (g_mkdir_with_parents(path, 0755) == 0)
        return path;

    g_free(path);

    // Finally try ~/.clippor
    path = g_strdup_printf("%s/.clippor", g_get_home_dir());

    if (g_mkdir_with_parents(path, 0755) == 0)
        return path;

    g_free(path);
    return NULL;
}

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

    ret = sqlite3_exec(db, statement, NULL, NULL, &err_msg);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed creating Version table: %s", err_msg
        );
        sqlite3_free(err_msg);

        return FALSE;
    }

    statement = "SELECT Db_version FROM Version;";
    sqlite3_stmt *stmt;
    ret = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);
    int db_version;

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed preparing SELECT statment: %s", sqlite3_errmsg(db)
        );
        return FALSE;
    }

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
        db_version = sqlite3_column_int(stmt, 0);
    else
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed stepping statment: %s", sqlite3_errmsg(db)
        );
        sqlite3_finalize(stmt);
        return FALSE;
    }

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

    store_dir = get_database_directory();

    if (store_dir == NULL)
    {
        g_set_error_literal(
            error, DATABASE_ERROR, DATABASE_ERROR_NO_DATA_DIR,
            "Cannot find a suitable directory for the database"
        );
        return FALSE;
    }

    char *db_path = g_strdup_printf("%s/history.sqlite3", store_dir);

    int ret = sqlite3_open(db_path, &db);
    const char *statement;
    char *err_msg;

    g_free(db_path);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "sqlite3_open() failed: %s", sqlite3_errmsg(db)
        );
        sqlite3_close(db);

        return FALSE;
    }

    // Setup main table where the history is stored
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

    ret = sqlite3_exec(db, statement, NULL, NULL, &err_msg);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "sqlite3_exec() failed: %s", err_msg
        );
        sqlite3_free(err_msg);
        sqlite3_close(db);

        return FALSE;
    }

    if (!update_database_version(error))
    {
        sqlite3_close(db);
        return FALSE;
    }

    return TRUE;
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

    int ret = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed preparing INSERT OR REPLACE statement: %s",
            sqlite3_errmsg(db)
        );
        return FALSE;
    }

    sqlite3_bind_text(stmt, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, data_id, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret != SQLITE_DONE)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed stepping statement: %s", sqlite3_errmsg(db)
        );
        sqlite3_finalize(stmt);
        return FALSE;
    }

    sqlite3_finalize(stmt);

    return TRUE;
}

/*
 * Write data to a persistent file. Returns the filename of the data file
 */
static char *
database_write_mime_type(const char *store_dir, GBytes *data, GError **error)
{
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
 * Creates a new entry in the database and writes data to a persistent location
 */
gboolean
database_add_entry(ClipporEntry *entry, GError **error)
{
    g_assert(CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    if (db == NULL)
        return TRUE;

    GHashTable *mime_types = clippor_entry_get_mime_types(entry);
    int64_t creation_time = clippor_entry_get_creation_time(entry);
    int64_t last_used_time = clippor_entry_get_last_used_time(entry);
    const char *id = clippor_entry_get_id(entry);
    const char *cb_label =
        clippor_clipboard_get_label(clippor_entry_get_clipboard(entry));

    // Id is used for the directory name to store all data for this entry
    g_autofree char *dirpath = g_strdup_printf("%s/data", store_dir);

    if (g_mkdir_with_parents(dirpath, 0755) == -1)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ADD,
            "Failed creating directory '%s': %s", dirpath, g_strerror(errno)
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
            database_write_mime_type(dirpath, data, &error);

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

    int ret = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed preparing INSERT OR REPLACE statement: %s",
            sqlite3_errmsg(db)
        );
        return FALSE;
    }

    sqlite3_bind_text(stmt, 2, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, mime_list->str, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, creation_time);
    sqlite3_bind_int64(stmt, 5, last_used_time);
    sqlite3_bind_int64(stmt, 6, clippor_entry_is_starred(entry));
    sqlite3_bind_text(stmt, 7, cb_label, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret != SQLITE_DONE)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed stepping statement: %s", sqlite3_errmsg(db)
        );
        sqlite3_finalize(stmt);
        return FALSE;
    }

    sqlite3_finalize(stmt);

    return TRUE;
}

/*
 * Derizalize entry at index in database to a ClipporEntry for clipboard. An
 * index of 0 indicates the most recent entry in the database. Note that users
 * may move entries around, so "recent" is not the best way to describe it.
 */
ClipporEntry *
database_deserialize_entry(ClipporClipboard *cb, uint64_t index, GError **error)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(error == NULL || *error == NULL);

    // Get row in Main table
    const char *statement = "SELECT Id, Mime_types, Creation_time,"
                            "Last_used_time, Starred FROM Main "
                            "WHERE Clipboard = ?"
                            "ORDER BY Position DESC "
                            "LIMIT 1 OFFSET ?";
    sqlite3_stmt *stmt;

    int ret = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed preparing SELECT statement: %s", sqlite3_errmsg(db)
        );
        return FALSE;
    }

    sqlite3_bind_text(
        stmt, 1, clippor_clipboard_get_label(cb), -1, SQLITE_STATIC
    );
    sqlite3_bind_int64(stmt, 2, index);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *mime_list = (const char *)sqlite3_column_text(stmt, 1);
        int64_t creation_time = sqlite3_column_int64(stmt, 2);
        int64_t last_used_time = sqlite3_column_int64(stmt, 3);
        gboolean starred = sqlite3_column_int(stmt, 4);

        ClipporEntry *entry = clippor_entry_new(NULL, creation_time, id, cb);

        g_object_set(
            entry, "starred", starred, "last-used-time", last_used_time, NULL
        );

        // Add mime types (without any data)
        gchar **mime_types = g_strsplit(mime_list, ",", -1);

        for (char **mime_type = mime_types; *mime_type != NULL; mime_type++)
            clippor_entry_add_mime_type(entry, *mime_type, NULL);

        g_strfreev(mime_types);

        sqlite3_finalize(stmt);
        return entry;
    }
    else if (ret == SQLITE_DONE)
        // No such row exists
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT,
            "No row exists at index %" PRIu64, index
        );
    else
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed stepping statement: %s", sqlite3_errmsg(db)
        );

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

    int ret = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed preparing SELECT statement: %s", sqlite3_errmsg(db)
        );
        return NULL;
    }

    sqlite3_bind_text(stmt, 1, clippor_entry_get_id(entry), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, mime_type, -1, SQLITE_STATIC);

    ret = sqlite3_step(stmt);

    if (ret == SQLITE_ROW)
    {
        const char *data_id = (const char *)sqlite3_column_text(stmt, 0);
        g_autofree char *filepath =
            g_strdup_printf("%s/data/%s", store_dir, data_id);

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
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed stepping statement: %s", sqlite3_errmsg(db)
        );

    sqlite3_finalize(stmt);

    return NULL;
}

/*
 * Get number of entries for clipboard in database. Returns -1 on error
 */
int
database_get_num_entries(ClipporClipboard *cb, GError **error)
{
    g_assert(CLIPPOR_IS_CLIPBOARD(cb));
    g_assert(error == NULL || *error == NULL);

    const char *statement = "SELECT COUNT(*) FROM Main WHERE Clipboard = ?;";
    sqlite3_stmt *stmt;

    int64_t ret = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed preparing SELECT statement: %s", sqlite3_errmsg(db)
        );
        return -1;
    }

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
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed stepping statement: %s", sqlite3_errmsg(db)
        );

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

    int64_t ret = sqlite3_prepare_v2(db, statement, -1, &stmt, NULL);

    if (ret != SQLITE_OK)
    {
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed preparing SELECT statement: %s", sqlite3_errmsg(db)
        );
        return -1;
    }

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
        g_set_error(
            error, DATABASE_ERROR, DATABASE_ERROR_SQLITE,
            "Failed stepping statement: %s", sqlite3_errmsg(db)
        );

    sqlite3_finalize(stmt);
    return -1;
}
