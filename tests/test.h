#include "clippor-entry.h"
#include <glib.h>

typedef struct
{
    GPid pid;
    char *display;
} WaylandCompositor;

void set_signal_handler(struct sigaction *sa);

void pre_startup(void);

WaylandCompositor *wayland_compositor_start(void);
void wayland_compositor_stop(WaylandCompositor *self);

void wl_copy(
    WaylandCompositor *wc, gboolean primary, const char *mime_type,
    char *format, ...
);
char *wl_paste(
    WaylandCompositor *wc, gboolean primary, gboolean newline,
    const char *mime_type
);

char *get_entry_contents(ClipporEntry *entry, const char *mime_type);
void cmp_entry(ClipporEntry *entry, ClipporEntry *entry2);

void server_instance_start(const char *config_contents);
void server_instance_dispatch(void);
void server_instance_run(void);
void server_instance_pause(void);
void server_instance_stop(void);
void server_instance_dispatch_and_run(void);
void server_instance_restart(void);
