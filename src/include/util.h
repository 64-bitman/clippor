#pragma once

#include <glib.h>
#include <stdint.h>

#define STRING(s) _STRING(s)
#define _STRING(s) #s

#define GFOREACH(item, list)                                                   \
    for (GList *__glist = list; __glist && (item = __glist->data, TRUE);       \
         __glist = __glist->next)

typedef struct _ClipporData ClipporData;

#define UTIL_ERROR (util_error_quark())

typedef enum
{
    UTIL_ERROR_SEND_DATA,
    UTIL_ERROR_RECEIVE_DATA,
    UTIL_ERROR_RMDIR
} UtilError;

GQuark util_error_quark(void);

gboolean
util_send_data(int32_t fd, ClipporData *data, int timeout, GError **error);
ClipporData *
util_receive_data(int32_t fd, int timeout, gboolean checksum, GError **error);

char *util_expand_env(const char *name);

gboolean util_remove_dir(const char *path, GError **error);

ClipporData *clippor_data_new(gboolean do_checksum);
ClipporData *
clippor_data_new_take(gconstpointer data, size_t size, gboolean do_checksum);
void clippor_data_unref(ClipporData *self);
ClipporData *clippor_data_ref(ClipporData *self);
void clippor_data_append(ClipporData *self, const uint8_t *bytes, uint size);
ClipporData *clippor_data_finish(ClipporData *self);
gconstpointer clippor_data_get_data(ClipporData *self, size_t *size);
const char *clippor_data_get_checksum(ClipporData *data);
int clippor_data_compare(ClipporData *data1, ClipporData *data2);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClipporData, clippor_data_unref)
