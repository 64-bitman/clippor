#pragma once

#include <glib.h>

gboolean server_start(const char *config_file, const char *data_directory);
void server_free(void);
