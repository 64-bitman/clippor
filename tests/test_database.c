#include "database.h"
#include "test.h"
#include <glib.h>
#include <locale.h>
#include <sqlite3.h>

typedef struct
{
} TestFixture;

static void
fixture_setup(TEST_UARGS)
{
    GError *error = NULL;
    g_assert_true(database_init(NULL, TRUE, &error));
    g_assert_no_error(error);
}

static void
fixture_teardown(TEST_UARGS)
{
    database_uninit();
}

/*
 * Test if creating a new entry row works properly
 */
static void
test_database_new_entry_row(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new_no_database(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE
    );
    g_autoptr(ClipporData) data = clippor_data_new(TRUE);

    clippor_data_append(data, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data);

    database_new_entry_row(entry, &error);
    g_assert_no_error(error);

    clippor_entry_set_mime_type(entry, "text/plain", data, &error);
    g_assert_no_error(error);

    g_autoptr(ClipporEntry) d_entry =
        database_get_entry_by_id(cb, clippor_entry_get_id(entry), &error);
    g_assert_no_error(error);

    cmp_entry(entry, d_entry);
}

/*
 * Test if updating an entry row works properly
 */
static void
test_database_update_entry_row(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );

    g_assert_no_error(error);

    g_object_set(entry, "starred", TRUE, NULL);

    g_assert_true(database_update_entry_row(entry, &error));
    g_assert_no_error(error);

    g_autoptr(ClipporEntry) d_entry =
        database_get_entry_by_id(cb, clippor_entry_get_id(entry), &error);
    g_assert_no_error(error);

    g_assert_true(clippor_entry_is_starred(d_entry));
}

/*
 * Test if entry in database can be moved to the front
 */
static void
test_database_set_entry_row(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");

    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);
    g_autoptr(ClipporEntry) entry2 = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    database_set_entry_row(entry, &error);
    g_assert_no_error(error);

    sqlite3 *db = database_get_db();

    sqlite3_stmt *stmt;

    g_assert_true(
        sqlite3_prepare_v2(
            db, "SELECT Id FROM Entries WHERE Position == 3;", -1, &stmt, NULL
        ) == SQLITE_OK
    );

    g_assert_true(sqlite3_step(stmt) == SQLITE_ROW);

    const char *id = (const char *)sqlite3_column_text(stmt, 0);

    g_assert_cmpstr(id, ==, clippor_entry_get_id(entry));

    sqlite3_finalize(stmt);
}

/*
 * Test if creating a row for a mime type works properly
 */
static void
test_database_new_mime_type_row(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    g_autoptr(ClipporData) data = clippor_data_new(TRUE);

    clippor_data_append(data, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data);

    g_assert_true(
        database_new_mime_type_row(entry, "text/plain", data, &error)
    );
    g_assert_no_error(error);

    sqlite3 *db = database_get_db();

    sqlite3_stmt *stmt;

    g_assert_true(
        sqlite3_prepare_v2(
            db,
            "SELECT Mime_type FROM Mime_types WHERE Mime_type = ? AND Id = ?;",
            -1, &stmt, NULL
        ) == SQLITE_OK
    );

    sqlite3_bind_text(stmt, 1, "text/plain", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, clippor_entry_get_id(entry), -1, SQLITE_STATIC);

    g_assert_true(sqlite3_step(stmt) == SQLITE_ROW);

    sqlite3_finalize(stmt);

    // Test if row is deleted
    g_assert_true(
        database_new_mime_type_row(entry, "text/plain", NULL, &error)
    );

    g_assert_true(
        sqlite3_prepare_v2(
            db,
            "SELECT Mime_type FROM Mime_types WHERE Mime_type = ? AND Id = ?;",
            -1, &stmt, NULL
        ) == SQLITE_OK
    );

    sqlite3_bind_text(stmt, 1, "text/plain", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, clippor_entry_get_id(entry), -1, SQLITE_STATIC);

    g_assert_true(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);
}

/*
 * Test if entry can be loaded through its index in the database.
 */
static void
test_database_get_entry_by_index(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);
    g_autoptr(ClipporEntry) entry2 = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    g_autoptr(ClipporData) data = clippor_data_new(TRUE);

    clippor_data_append(data, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data);

    clippor_entry_set_mime_type(entry, "text/plain", data, &error);
    g_assert_no_error(error);
    clippor_entry_set_mime_type(entry2, "text/plain", data, &error);
    g_assert_no_error(error);

    g_autoptr(ClipporEntry) d_entry =
        database_get_entry_by_index(cb, 1, &error);
    g_assert_no_error(error);
    g_autoptr(ClipporEntry) d_entry2 =
        database_get_entry_by_index(cb, 0, &error);
    g_assert_no_error(error);

    cmp_entry(entry, d_entry);
    cmp_entry(entry2, d_entry2);
}

/*
 * Test if entry can be loaded through its id in the database.
 */
static void
test_database_get_entry_by_id(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);
    g_autoptr(ClipporEntry) entry2 = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    g_autoptr(ClipporData) data = clippor_data_new(TRUE);

    clippor_data_append(data, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data);

    clippor_entry_set_mime_type(entry, "text/plain", data, &error);
    g_assert_no_error(error);
    clippor_entry_set_mime_type(entry2, "text/plain", data, &error);
    g_assert_no_error(error);

    g_autoptr(ClipporEntry) d_entry =
        database_get_entry_by_id(cb, clippor_entry_get_id(entry), &error);
    g_assert_no_error(error);
    g_autoptr(ClipporEntry) d_entry2 =
        database_get_entry_by_id(cb, clippor_entry_get_id(entry2), &error);
    g_assert_no_error(error);

    cmp_entry(entry, d_entry);
    cmp_entry(entry2, d_entry2);

    g_assert_null(database_get_entry_by_id(cb, "UNKNOWN", &error));
    g_assert_error(error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT);
    g_clear_error(&error);
}

/*
 * Test if data can be loaded by querying the mime type in the database.
 */
static void
test_database_get_entry_mime_type_data(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    g_autoptr(ClipporData) data = clippor_data_new(TRUE);

    clippor_data_append(data, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data);

    g_assert_true(
        database_new_mime_type_row(entry, "text/plain", data, &error)
    );
    g_assert_no_error(error);

    g_autoptr(ClipporData) d_data =
        database_get_entry_mime_type_data(entry, "text/plain", &error);
    g_assert_no_error(error);

    g_assert_true(clippor_data_compare(data, d_data) == 0);
}

/*
 * Test if mime type row can be removed by id in the database.
 */
static void
test_database_remove_mime_type_row_by_id(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    g_autoptr(ClipporData) data = clippor_data_new(TRUE);

    clippor_data_append(data, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data);

    g_assert_true(
        database_new_mime_type_row(entry, "text/plain", data, &error)
    );
    g_assert_no_error(error);

    g_assert_true(database_remove_mime_type_row_by_id(
        clippor_entry_get_id(entry), "text/plain", &error
    ));
    g_assert_no_error(error);

    sqlite3 *db = database_get_db();

    sqlite3_stmt *stmt;

    g_assert_true(
        sqlite3_prepare_v2(
            db,
            "SELECT Mime_type FROM Mime_types WHERE Mime_type = ? AND Id = ?;",
            -1, &stmt, NULL
        ) == SQLITE_OK
    );

    sqlite3_bind_text(stmt, 1, "text/plain", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, clippor_entry_get_id(entry), -1, SQLITE_STATIC);

    g_assert_true(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);

    // Check if data row is removed
    g_assert_true(
        sqlite3_prepare_v2(
            db, "SELECT Data_id FROM Data WHERE Data_Id = ?;", -1, &stmt, NULL
        ) == SQLITE_OK
    );

    sqlite3_bind_text(
        stmt, 1, clippor_data_get_checksum(data), -1, SQLITE_STATIC
    );

    g_assert_true(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);
}

/*
 * Test if entry row can be removed by id in the database.
 */
static void
test_database_remove_entry_row_by_id(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    g_autoptr(ClipporData) data = clippor_data_new(TRUE);

    clippor_data_append(data, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data);

    g_assert_true(
        database_new_mime_type_row(entry, "text/plain", data, &error)
    );
    g_assert_no_error(error);

    g_assert_true(
        database_remove_entry_row_by_id(clippor_entry_get_id(entry), &error)
    );
    g_assert_no_error(error);

    sqlite3 *db = database_get_db();

    sqlite3_stmt *stmt;

    g_assert_true(
        sqlite3_prepare_v2(
            db, "SELECT Id FROM Entries WHERE Id = ?;", -1, &stmt, NULL
        ) == SQLITE_OK
    );

    sqlite3_bind_text(stmt, 1, clippor_entry_get_id(entry), -1, SQLITE_STATIC);

    g_assert_true(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);

    g_assert_true(
        sqlite3_prepare_v2(
            db,
            "SELECT Mime_type FROM Mime_types WHERE Mime_type = ? AND Id = ?;",
            -1, &stmt, NULL
        ) == SQLITE_OK
    );

    sqlite3_bind_text(stmt, 1, "text/plain", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, clippor_entry_get_id(entry), -1, SQLITE_STATIC);

    g_assert_true(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);

    // Check if data row is removed
    g_assert_true(
        sqlite3_prepare_v2(
            db, "SELECT Data_id FROM Data WHERE Data_Id = ?;", -1, &stmt, NULL
        ) == SQLITE_OK
    );

    sqlite3_bind_text(
        stmt, 1, clippor_data_get_checksum(data), -1, SQLITE_STATIC
    );

    g_assert_true(sqlite3_step(stmt) == SQLITE_DONE);

    sqlite3_finalize(stmt);
}

/*
 * Test if entry rows are trimmed according to clipboard
 */
static void
test_database_trim_entry_rows(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");

    g_object_set(cb, "max-entries", 5, NULL);

    for (int i = 0; i < 10; i++)
    {
        g_autoptr(ClipporEntry) entry = clippor_entry_new(
            NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
        );
        g_assert_no_error(error);
    }

    g_assert_true(database_trim_entry_rows(cb, FALSE, &error));

    g_assert_cmpint(database_get_num_entries(cb, NULL), ==, 5);

    g_assert_true(database_trim_entry_rows(cb, TRUE, &error));

    g_assert_cmpint(database_get_num_entries(cb, NULL), ==, 0);
}

/*
 * Test if mime type row is updated in database
 */
static void
test_database_update_mime_type_row(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    g_autoptr(ClipporData) data = clippor_data_new(TRUE);

    clippor_data_append(data, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data);

    g_assert_true(
        database_new_mime_type_row(entry, "text/plain", data, &error)
    );

    g_autoptr(ClipporData) data2 = clippor_data_new(TRUE);

    clippor_data_append(data2, (const uint8_t *)"TEST", 4);
    clippor_data_finish(data2);

    g_assert_true(
        database_update_mime_type_row(entry, "text/plain", data2, &error)
    );

    sqlite3 *db = database_get_db();

    sqlite3_stmt *stmt;

    g_assert_true(
        sqlite3_prepare_v2(
            db,
            "SELECT Data_id FROM Mime_types WHERE Mime_type = ? AND Id = ?;",
            -1, &stmt, NULL
        ) == SQLITE_OK
    );

    sqlite3_bind_text(stmt, 1, "text/plain", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, clippor_entry_get_id(entry), -1, SQLITE_STATIC);

    g_assert_true(sqlite3_step(stmt) == SQLITE_ROW);

    const char *data_id = (const char *)sqlite3_column_text(stmt, 0);

    g_assert_cmpstr(data_id, ==, clippor_data_get_checksum(data2));

    sqlite3_finalize(stmt);

    g_assert_false(
        database_update_mime_type_row(entry, "UNKNOWN", data2, &error)
    );
    g_assert_error(error, DATABASE_ERROR, DATABASE_ERROR_ROW_NONEXISTENT);
    g_clear_error(&error);
}

/*
 * Test if number of entries for clipboard is correct
 */
static void
test_database_get_num_entries(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");

    g_object_set(cb, "max-entries", 5, NULL);

    for (int i = 0; i < 10; i++)
    {
        g_autoptr(ClipporEntry) entry = clippor_entry_new(
            NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
        );
        g_assert_no_error(error);
    }

    g_assert_cmpint(database_get_num_entries(cb, &error), ==, 10);
    g_assert_no_error(error);

    g_assert_true(database_trim_entry_rows(cb, TRUE, &error));
    g_assert_no_error(error);

    g_assert_cmpint(database_get_num_entries(cb, &error), ==, 0);
    g_assert_no_error(error);
}

/*
 * Test if checking if entry exists via id works properly
 */
static void
test_database_entry_id_exists(TEST_UARGS)
{
    GError *error = NULL;
    g_autoptr(ClipporClipboard) cb = clippor_clipboard_new("Test");
    g_autoptr(ClipporEntry) entry = clippor_entry_new(
        NULL, -1, NULL, cb, CLIPPOR_SELECTION_TYPE_NONE, &error
    );
    g_assert_no_error(error);

    const char *id = clippor_entry_get_id(entry);

    g_assert_cmpint(database_entry_id_exists(id, &error), ==, 0);
    g_assert_no_error(error);

    g_assert_cmpint(database_entry_id_exists("unknown", &error), ==, 1);
    g_assert_no_error(error);
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_test_init(&argc, &argv, NULL);

    pre_startup();

    struct sigaction sa;
    set_signal_handler(&sa);

    TEST_ADD("/database/new-entry-row", test_database_new_entry_row);
    TEST_ADD("/database/update-entry-row", test_database_update_entry_row);
    TEST_ADD("/database/set-entry-row", test_database_set_entry_row);
    TEST_ADD("/database/new-mime-type-row", test_database_new_mime_type_row);
    TEST_ADD("/database/get-entry-by-index", test_database_get_entry_by_index);
    TEST_ADD("/database/get-entry-by-id", test_database_get_entry_by_id);
    TEST_ADD(
        "/database/get-entry-mime-type-data",
        test_database_get_entry_mime_type_data
    );
    TEST_ADD(
        "/database/remove-mime-type-row-by-id",
        test_database_remove_mime_type_row_by_id
    );
    TEST_ADD(
        "/database/remove-entry-row-by-id", test_database_remove_entry_row_by_id
    );
    TEST_ADD("/database/trim-entry-rows", test_database_trim_entry_rows);
    TEST_ADD(
        "/database/update-mime-type-row", test_database_update_mime_type_row
    );
    TEST_ADD("/database/get-num-entries", test_database_get_num_entries);
    TEST_ADD("/database/entry-id-exists", test_database_entry_id_exists);

    return g_test_run();
}
