#pragma once

#include <glib.h>

typedef enum
{
    SERVER_FLAG_NONE = 0,
    SERVER_FLAG_USE_CONFIG_FILE = 1 << 0,
    SERVER_FLAG_DB_IN_MEMORY = 1 << 1,
    SERVER_FLAG_NO_INIT_DB = 1 << 2,
    SERVER_FLAG_NO_UNINIT_DB = 1 << 3,
    SERVER_FLAG_MANUAL = 1 << 4,
} ServerFlags;

void server_free(uint flags);
gboolean
server_start(const char *config_value, const char *data_directory, uint flags);

GMainLoop *server_get_main_loop(void);
const GPtrArray *server_get_clipboards(void);
void server_set_main_context(GMainContext *context);
GMainContext *server_get_main_context(void);
