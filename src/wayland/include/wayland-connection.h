#pragma once

#include "clippor-selection.h"
#include "wayland-seat.h"
#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(
    WaylandConnection, wayland_connection, WAYLAND, CONNECTION, GObject
)
#define WAYLAND_TYPE_CONNECTION (wayland_connection_get_type())

typedef enum
{
    WAYLAND_ERROR_CONNECT,
    WAYLAND_ERROR_FLUSH,
    WAYLAND_ERROR_DISPATCH,
    WAYLAND_ERROR_ROUNDTRIP,
    WAYLAND_ERROR_TIMEOUT,
    WAYLAND_ERROR_NOT_CONNECTED,
    WAYLAND_ERROR_CREATE_SEAT,
    WAYLAND_ERROR_NO_DATA_PROTOCOL
} WaylandError;

#define WAYLAND_ERROR (wayland_error_quark())
GQuark wayland_error_quark(void);

typedef struct WaylandDataDeviceManager WaylandDataDeviceManager;
typedef struct WaylandDataDevice WaylandDataDevice;
typedef struct WaylandDataSource WaylandDataSource;
typedef struct WaylandDataOffer WaylandDataOffer;

typedef struct
{
    void (*data_offer)(
        void *data, WaylandDataDevice *device, WaylandDataOffer *offer
    );
    void (*selection)(
        void *data, WaylandDataDevice *device, WaylandDataOffer *offer,
        ClipporSelectionType selection
    );
    void (*finished)(void *data, WaylandDataDevice *device);
} WaylandDataDeviceListener;

typedef struct
{
    void (*send)(
        void *data, WaylandDataSource *source, const char *mime_type, int fd
    );
    void (*cancelled)(void *data, WaylandDataSource *source);
} WaylandDataSourceListener;

typedef struct
{
    // Return TRUE to add mime type to array
    gboolean (*offer)(
        void *data, WaylandDataOffer *offer, const char *mime_type
    );
} WaylandDataOfferListener;

WaylandConnection *wayland_connection_new(const char *display);

gboolean wayland_connection_start(WaylandConnection *self, GError **error);
void wayland_connection_stop(WaylandConnection *self);

int wayland_connection_get_fd(WaylandConnection *self);
gboolean wayland_connection_is_active(WaylandConnection *self);
const char *wayland_connection_get_display(WaylandConnection *self);
WaylandSeat *
wayland_connection_get_seat(WaylandConnection *self, const char *name);

gboolean wayland_connection_flush(WaylandConnection *self, GError **error);
int wayland_connection_dispatch(WaylandConnection *self, GError **error);
gboolean wayland_connection_roundtrip(WaylandConnection *self, GError **error);

void wayland_connection_install_source(
    WaylandConnection *self, GMainContext *context
);
void wayland_connection_uninstall_source(WaylandConnection *self);

// Wayland data proxy functions

gboolean wayland_data_device_manager_is_valid(WaylandDataDeviceManager *self);
gboolean wayland_data_device_is_valid(WaylandDataDevice *self);
gboolean wayland_data_source_is_valid(WaylandDataSource *self);
gboolean wayland_data_offer_is_valid(WaylandDataOffer *self);

// Creator functions

WaylandDataDeviceManager *wayland_connection_get_data_device_manager(
    WaylandConnection *self, ClipporSelectionTypeFlags *sels
);
WaylandDataDevice *wayland_data_device_manager_get_data_device(
    WaylandDataDeviceManager *self, WaylandSeat *seat
);
WaylandDataSource *
wayland_data_device_manager_create_data_source(WaylandDataDeviceManager *self);
WaylandDataOffer *
wayland_data_device_wrap_offer_proxy(WaylandDataDevice *self, void *proxy);

// Reference management functions

void wayland_data_device_destroy(WaylandDataDevice *self);
void wayland_data_source_destroy(WaylandDataSource *self);
void wayland_data_offer_destroy(WaylandDataOffer *self);
void wayland_data_device_manager_discard(WaylandDataDeviceManager *self);

// Listener functions

void wayland_data_device_add_listener(
    WaylandDataDevice *self, const WaylandDataDeviceListener *listener,
    void *data
);
void wayland_data_source_add_listener(
    WaylandDataSource *self, const WaylandDataSourceListener *listener,
    void *data
);
void wayland_data_offer_add_listener(
    WaylandDataOffer *self, const WaylandDataOfferListener *listener, void *data
);

// Data proxy methods

void wayland_data_device_set_selection(
    WaylandDataDevice *self, WaylandDataSource *source,
    ClipporSelectionType selection
);
void wayland_data_source_offer(WaylandDataSource *self, const char *mime_type);
void wayland_data_offer_receive(
    WaylandDataOffer *self, const char *mime_type, int fd
);
GPtrArray *wayland_data_offer_get_mime_types(WaylandDataOffer *self);

// Cleanup
G_DEFINE_AUTOPTR_CLEANUP_FUNC(
    WaylandDataDeviceManager, wayland_data_device_manager_discard
)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaylandDataDevice, wayland_data_device_destroy);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaylandDataSource, wayland_data_source_destroy);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaylandDataOffer, wayland_data_offer_destroy);
