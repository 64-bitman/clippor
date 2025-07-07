#include <glib.h>

#define STRING(s) _STRING(s)
#define _STRING(s) #s

#define GFOREACH(item, list)                                                   \
    for (GList *__glist = list; __glist && (item = __glist->data, TRUE);       \
         __glist = __glist->next)

#define UTIL_ERROR (util_error_quark())

typedef enum
{
    UTIL_ERROR_SEND_DATA,
    UTIL_ERROR_RECEIVE_DATA
} UtilError;

GQuark util_error_quark(void);

gboolean util_send_data(int32_t fd, GBytes *data, int timeout, GError **error);
GBytes *util_receive_data(int32_t fd, int timeout, GError **error);

char *util_expand_env(const char *name);
