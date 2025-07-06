#include "util.h"
#include <fcntl.h>
#include <glib.h>
#include <inttypes.h>
#include <unistd.h>

G_DEFINE_QUARK(util_error_quark, util_error)

gboolean
util_send_data(int32_t fd, GBytes *data, int timeout, GError **error)
{
    g_assert(data != NULL);
    g_assert(fd >= 0);
    g_assert(error == NULL || *error == NULL);

    size_t length;
    const char *stuff = g_bytes_get_data(data, &length);

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

GBytes *
util_receive_data(int32_t fd, int timeout, GError **error)
{
    g_assert(error == NULL || *error == NULL);
    g_assert(fd >= 0);

    GByteArray *data = g_byte_array_new();
    GPollFD pfd = {.fd = fd, .events = G_IO_IN};

    // Make pipe (read end) non-blocking
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == -1)
    {
        g_set_error(
            error, UTIL_ERROR, UTIL_ERROR_RECEIVE_DATA, "fcntl() failed: %s",
            g_strerror(errno)
        );
        g_byte_array_unref(data);
        return NULL;
    }

    uint8_t *bytes = g_malloc(8192);
    ssize_t r = 0;
    gboolean err = FALSE;

    // Only poll before reading when we first start, then we do non-blocking
    // reads and check for EAGAIN or EINTR to signal to poll again.
    goto poll_data;

    while (errno = 0, TRUE)
    {
        r = read(fd, bytes, 8192);

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
                    g_set_error(
                        error, UTIL_ERROR, UTIL_ERROR_RECEIVE_DATA,
                        "g_poll() failed: %s", g_strerror(errno)
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
        g_byte_array_append(data, bytes, r);
    }
    g_free(bytes);

    if (err)
    {
        g_byte_array_unref(data);
        return NULL;
    }

    return g_byte_array_free_to_bytes(data);
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
