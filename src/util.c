#include "util.h"
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <inttypes.h>
#include <unistd.h>

G_DEFINE_QUARK(util_error_quark, util_error)

// Represents the data of a mime type, including its checksum
struct _ClipporData
{
    GByteArray *byte_array; // If NULL then data has been finished, cannot
                            // modify anymore.
    GBytes *bytes;
    GChecksum *checksum; // NULL if not computing checksum
    grefcount ref_count;
};

gboolean
util_send_data(int32_t fd, ClipporData *data, int timeout, GError **error)
{
    g_assert(data != NULL);
    g_assert(fd >= 0);
    g_assert(error == NULL || *error == NULL);

    size_t length;
    const char *stuff = clippor_data_get_data(data, &length);

    GPollFD pfd = {.fd = fd, .events = G_IO_OUT};
    ssize_t written = 0;
    size_t total = 0;

    while (errno = 0, total < length && g_poll(&pfd, 1, timeout) > 0)
    {
        written = write(fd, stuff + total, length - total);

        if (written == -1)
        {
            g_set_error(
                error, UTIL_ERROR, UTIL_ERROR_SEND_DATA, "write() failed: %s",
                g_strerror(errno)
            );
            break;
        }
        total += written;
    }

    if (errno != 0)
        g_set_error(
            error, UTIL_ERROR, UTIL_ERROR_SEND_DATA, "g_poll() failed: %s",
            g_strerror(errno)
        );

    if (*error != NULL)
        return FALSE;

    return TRUE;
}

ClipporData *
util_receive_data(int32_t fd, int timeout, gboolean checksum, GError **error)
{
    g_assert(fd >= 0);
    g_assert(error == NULL || *error == NULL);

    ClipporData *data = clippor_data_new(checksum);
    GPollFD pfd = {.fd = fd, .events = G_IO_IN};

    // Make pipe (read end) non-blocking
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == -1)
    {
        g_set_error(
            error, UTIL_ERROR, UTIL_ERROR_RECEIVE_DATA, "fcntl() failed: %s",
            g_strerror(errno)
        );
        clippor_data_unref(data);
        return NULL;
    }

    uint8_t *bytes = g_malloc(4096);
    ssize_t r = 0;
    gboolean err = FALSE;

    // Only poll before reading when we first start, then we do non-blocking
    // reads and check for EAGAIN or EINTR to signal to poll again.
    goto poll_data;

    while (errno = 0, TRUE)
    {
        r = read(fd, bytes, 4096);

        if (r == 0)
            break;
        else if (r < 0)
        {
            if (errno == EAGAIN || errno == EINTR)
            {
poll_data:
                if (g_poll(&pfd, 1, timeout) > 0)
                    continue;
                else
                    g_set_error_literal(
                        error, UTIL_ERROR, UTIL_ERROR_RECEIVE_DATA,
                        "g_poll() failed or timed out"
                    );
                err = TRUE;
                break;
            }
            g_set_error(
                error, UTIL_ERROR, UTIL_ERROR_RECEIVE_DATA, "read() failed: %s",
                g_strerror(errno)
            );
            err = TRUE;
            break;
        }
        clippor_data_append(data, bytes, r);
    }
    g_free(bytes);

    if (err)
    {
        clippor_data_unref(data);
        return NULL;
    }

    clippor_data_finish(data);
    return data;
}

/*
 * Return value of environemnt variable in form of $<NAME>. If string doesn't
 * start with '$' or is a non-existent environment variable, then return
 * unchanged but in a newly alloced string.
 */
char *
util_expand_env(const char *name)
{
    g_assert(name != NULL);

    if (name[0] != '$')
        return g_strdup(name);

    const char *val = g_getenv(name + 1);

    if (val == NULL)
        return g_strdup(name);

    return g_strdup(val);
}

/*
 * Removes directory recursively, without following symlinks.
 */
gboolean
util_remove_dir(const char *path, GError **error)
{
    g_assert(path != NULL);
    g_assert(error == NULL || *error == NULL);

    g_autoptr(GQueue) stack = g_queue_new();
    g_autoptr(GPtrArray) dirs_to_delete =
        g_ptr_array_new_with_free_func(g_object_unref);

    g_autoptr(GFile) root = g_file_new_for_path(path);
    g_queue_push_head(stack, g_object_ref(root));

    while (!g_queue_is_empty(stack))
    {
        g_autoptr(GFile) dir = g_queue_pop_head(stack);

        g_autoptr(GFileEnumerator) enumerator = g_file_enumerate_children(
            dir,
            G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
            G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error
        );

        if (!enumerator)
            return FALSE;

        GFileInfo *info;
        while ((info = g_file_enumerator_next_file(enumerator, NULL, error)) !=
               NULL)
        {
            g_autoptr(GFileInfo) info_holder = info; // auto-free

            const char *name = g_file_info_get_name(info);
            GFileType type = g_file_info_get_file_type(info);
            g_autoptr(GFile) child = g_file_get_child(dir, name);

            if (type == G_FILE_TYPE_DIRECTORY)
                g_queue_push_head(stack, g_object_ref(child));
            else if (!g_file_delete(child, NULL, error))
                return FALSE;
        }

        // Save this dir for deletion after its children
        g_ptr_array_add(dirs_to_delete, g_object_ref(dir));
    }

    // Delete directories in post-order
    for (gssize i = dirs_to_delete->len - 1; i >= 0; i--)
    {
        GFile *dir = g_ptr_array_index(dirs_to_delete, i);
        if (!g_file_delete(dir, NULL, error))
            return FALSE;
    }

    return TRUE;
}

ClipporData *
clippor_data_new(gboolean do_checksum)
{
    ClipporData *data = g_new0(ClipporData, 1);

    data->byte_array = g_byte_array_new();

    g_ref_count_init(&data->ref_count);

    if (do_checksum)
        data->checksum = g_checksum_new(G_CHECKSUM_SHA1);

    return data;
}

ClipporData *
clippor_data_new_take(gpointer data, size_t size, gboolean do_checksum)
{
    ClipporData *new = clippor_data_new(do_checksum);

    clippor_data_append(new, data, size);
    return clippor_data_finish(new);
}

void
clippor_data_unref(ClipporData *self)
{
    if (self == NULL)
        return;

    if (g_ref_count_dec(&self->ref_count))
    {
        if (self->byte_array != NULL)
            g_byte_array_unref(self->byte_array);
        if (self->bytes != NULL)
            g_bytes_unref(self->bytes);
        if (self->checksum != NULL)
            g_checksum_free(self->checksum);
        g_free(self);
    }
}

ClipporData *
clippor_data_ref(ClipporData *self)
{
    g_assert(self != NULL);
    g_ref_count_inc(&self->ref_count);
    return self;
}

void
clippor_data_append(ClipporData *self, const uint8_t *bytes, uint size)
{
    g_assert(self != NULL);
    g_assert(bytes != NULL);

    g_byte_array_append(self->byte_array, bytes, size);

    if (self->checksum != NULL)
        g_checksum_update(self->checksum, bytes, size);
}

ClipporData *
clippor_data_finish(ClipporData *self)
{
    g_assert(self != NULL);

    self->bytes = g_byte_array_free_to_bytes(self->byte_array);
    self->byte_array = NULL;

    return self;
}

gconstpointer
clippor_data_get_data(ClipporData *self, size_t *size)
{
    g_assert(self != NULL);
    g_assert(self->bytes != NULL);
    return g_bytes_get_data(self->bytes, size);
}

const char *
clippor_data_get_checksum(ClipporData *self)
{
    g_assert(self != NULL);
    g_assert(self->checksum != NULL);

    return g_checksum_get_string(self->checksum);
}
