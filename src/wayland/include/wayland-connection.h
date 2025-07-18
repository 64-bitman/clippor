#pragma once

#include "clippor-clipboard.h"
#include <glib-object.h>
#include <wayland-client.h>

#define WAYLAND_CONNECTION_ERROR (wayland_connection_error_quark())

typedef enum
{
    WAYLAND_CONNECTION_ERROR_CONNECT,
    WAYLAND_CONNECTION_ERROR_FLUSH,
    WAYLAND_CONNECTION_ERROR_DISPATCH,
    WAYLAND_CONNECTION_ERROR_ROUNDTRIP,
    WAYLAND_CONNECTION_ERROR_TIMEOUT,
    WAYLAND_CONNECTION_ERROR_NO_RUNTIME,
    WAYLAND_CONNECTION_ERROR_ENV_NOT_SET,
} WaylandConnectionError;

GQuark wayland_connection_error_quark(void);

#define WAYLAND_TYPE_CONNECTION (wayland_connection_get_type())

G_DECLARE_FINAL_TYPE(
    WaylandConnection, wayland_connection, WAYLAND, CONNECTION, GObject
)

typedef struct WaylandDataDeviceManager WaylandDataDeviceManager;
typedef struct WaylandDataDevice WaylandDataDevice;
typedef struct WaylandDataSource WaylandDataSource;
typedef struct WaylandDataOffer WaylandDataOffer;

typedef struct
{
    void (*data_offer)(
        void *data, WaylandDataDevice *device, WaylandDataOffer *offer
    );
    // Offer can be NULL to indicate selection is cleared/empty
    void (*selection)(
        void *data, WaylandDataDevice *device, WaylandDataOffer *offer,
        ClipporSelectionType selection
    );
    void (*finished)(void *data, WaylandDataDevice *device);
} WaylandDataDeviceListener;

typedef struct
{
    // File descriptor will automatically be closed after callback is done
    void (*send)(
        void *data, WaylandDataSource *source, const char *mime_type, int32_t fd
    );
    void (*cancelled)(void *data, WaylandDataSource *source);
} WaylandDataSourceListener;

typedef struct
{
    gboolean (*offer)(
        void *data, WaylandDataOffer *offer, const char *mime_type
    );
} WaylandDataOfferListener;

WaylandConnection *
wayland_connection_new(const char *display_name, GError **error);

typedef struct _WaylandSeat WaylandSeat;

int wayland_connection_get_fd(WaylandConnection *self);
WaylandSeat *
wayland_connection_get_seat(WaylandConnection *self, const char *name);
WaylandSeat *
wayland_connection_match_seat(WaylandConnection *self, GRegex *pattern);
GHashTable *wayland_connection_get_seats(WaylandConnection *self);
char *wayland_connection_get_display_name(WaylandConnection *self);
struct wl_display *wayland_connection_get_display(WaylandConnection *self);

gboolean wayland_connection_flush(WaylandConnection *ct, GError **error);
int wayland_connection_dispatch(WaylandConnection *ct, GError **error);
gboolean wayland_connection_roundtrip(WaylandConnection *ct, GError **error);

void wayland_connection_install_source(
    WaylandConnection *self, GMainContext *context
);
void wayland_connection_uninstall_source(WaylandConnection *self);

WaylandDataDeviceManager *
wayland_connection_get_data_device_manager(WaylandConnection *self);

gboolean wayland_data_device_manager_is_valid(
    WaylandDataDeviceManager *data_device_manager
);
gboolean wayland_data_device_is_valid(WaylandDataDevice *data_device);
gboolean wayland_data_source_is_valid(WaylandDataSource *data_source);
gboolean wayland_data_offer_is_valid(WaylandDataOffer *data_offer);

void wayland_data_device_destroy(WaylandDataDevice *device);
void wayland_data_source_destroy(WaylandDataSource *source);
void wayland_data_offer_destroy(WaylandDataOffer *offer);
void wayland_data_device_manager_discard(WaylandDataDeviceManager *manager);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaylandDataDevice, wayland_data_device_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaylandDataSource, wayland_data_source_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaylandDataOffer, wayland_data_offer_destroy)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(
    WaylandDataDeviceManager, wayland_data_device_manager_discard
)

WaylandDataDevice *wayland_data_device_manager_get_data_device(
    WaylandDataDeviceManager *manager, WaylandSeat *seat
);

WaylandDataSource *wayland_data_device_manager_create_data_source(
    WaylandDataDeviceManager *manager
);

void wayland_data_device_set_seletion(
    WaylandDataDevice *device, WaylandDataSource *source,
    ClipporSelectionType selection
);
void
wayland_data_source_offer(WaylandDataSource *source, const char *mime_type);
void wayland_data_offer_receive(
    WaylandDataOffer *offer, const char *mime_type, int32_t fd
);

void wayland_data_device_add_listener(
    WaylandDataDevice *device, WaylandDataDeviceListener *listener, void *data
);

void wayland_data_source_add_listener(
    WaylandDataSource *source, WaylandDataSourceListener *listener, void *data
);

void wayland_data_offer_add_listener(
    WaylandDataOffer *offer, WaylandDataOfferListener *listener, void *data
);

GPtrArray *wayland_data_offer_get_mime_types(WaylandDataOffer *offer);

void wayland_connection_free_static(void);
