#include "wayland-connection.h"
#include "wayland-seat.h"
#include <glib.h>
#include <gmodule.h>

typedef struct
{
    gboolean available;

    WaylandConnection *(*connection_new)(const char *);
    gboolean (*connection_start)(WaylandConnection *, GError **);
    gboolean (*connection_stop)(WaylandConnection *);
    void (*connection_install_source)(WaylandConnection *, GMainContext *);
    WaylandSeat *(*connection_get_seat)(WaylandConnection *, const char *);

    WaylandSelection *(*seat_get_selection)(
        WaylandSeat *self, ClipporSelectionType
    );
} WaylandModule;

extern WaylandModule WAYLAND_FUNCS;

void modules_init(void);
void modules_uninit(void);
