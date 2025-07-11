#include <glib.h>

typedef struct
{
    GPid pid;
    char *display;
} WaylandCompositor;

typedef struct
{
    GThread *thread;
    GMainLoop *loop;
    char *test_name;
    char *config_file;
    char *data_directory;
    gboolean stopped;
    struct sigaction sa;
} ServerInstance;

void set_signal_handler(struct sigaction *sa);

WaylandCompositor *wayland_compositor_start(void);
void wayland_compositor_stop(WaylandCompositor *self);

void wl_copy(WaylandCompositor *wc, gboolean primary, char *format, ...);
char *wl_paste(
    WaylandCompositor *wc, gboolean primary, gboolean newline, char *mime_type
);

ServerInstance *run_server(const char *test_name, const char *config_contents);
gboolean stop_server(ServerInstance *server);
gboolean restart_server(ServerInstance *server);
