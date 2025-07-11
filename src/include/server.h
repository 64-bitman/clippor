#pragma once

#include <glib.h>

gboolean server_start(const char *config_file, const char *data_directory);

GMainLoop *server_get_main_loop(void);
const GPtrArray *server_get_clipboards(void);
gboolean server_is_running(void);
