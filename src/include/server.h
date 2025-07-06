#pragma once

#include <glib.h>

gboolean server_start(void);

GMainLoop *server_get_main_loop(void);
