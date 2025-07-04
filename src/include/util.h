#include <glib.h>

#define UTIL_ERROR (util_error_quark())

typedef enum
{
    UTIL_ERROR_SEND_DATA,
    UTIL_ERROR_RECEIVE_DATA
} UtilError;

GQuark util_error_quark(void);

gboolean util_send_data(int32_t fd, GBytes *data, int timeout, GError **error);
GBytes *util_receive_data(int32_t fd, int timeout, GError **error);
