#include "test.h"
#include "wayland-connection.h"
#include "wayland-seat.h"
#include <glib.h>
#include <locale.h>

/*
 * Test if Wayland connection starts up correctly
 */
static void
test_wayland_connection_startup(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    GError *error = NULL;
    g_autoptr(WaylandConnection) ct =
        wayland_connection_new(wc->display, &error);

    g_assert_no_error(error);
    g_assert_nonnull(ct);

    g_assert_cmpstr(wayland_connection_get_display_name(ct), ==, wc->display);

    wayland_compositor_stop(wc);
    wayland_connection_free_static();
}

/*
 * Test if seat can be retrieved from connection
 */
static void
test_wayland_connection_get_seat(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    GError *error = NULL;
    g_autoptr(WaylandConnection) ct =
        wayland_connection_new(wc->display, &error);

    g_assert_no_error(error);
    g_assert_nonnull(ct);

    WaylandSeat *seat = wayland_connection_get_seat(ct, NULL);

    g_assert_nonnull(seat);

    const char *seat_name = wayland_seat_get_name(seat);
    g_autoptr(GRegex) regex =
        g_regex_new(seat_name, G_REGEX_DEFAULT, G_REGEX_MATCH_DEFAULT, &error);
    g_assert_no_error(error);

    WaylandSeat *match = wayland_connection_match_seat(ct, regex);

    g_assert_nonnull(match);

    wayland_compositor_stop(wc);
    wayland_connection_free_static();
}

static gboolean
wayland_data_offer_event_offer(
    void *data, WaylandDataOffer *offer, const char *mime_type
)
{
    g_assert_nonnull(data);
    g_assert_nonnull(offer);
    g_assert_nonnull(mime_type);

    *(uint *)data += 5;
    return TRUE;
}

static WaylandDataOfferListener offer_listener = {
    .offer = wayland_data_offer_event_offer
};

static void
wayland_data_device_event_data_offer(
    void *data, WaylandDataDevice *device, WaylandDataOffer *offer
)
{
    g_assert_nonnull(data);
    g_assert_nonnull(device);
    g_assert_nonnull(offer);

    *(uint *)data += 10;
    wayland_data_offer_add_listener(offer, &offer_listener, data);
}
static void
wayland_data_device_event_selection(
    void *data, WaylandDataDevice *device, WaylandDataOffer *offer,
    ClipporSelectionType selection
)
{
    g_assert_nonnull(data);
    g_assert_nonnull(device);
    g_assert_nonnull(offer);
    g_assert_cmpint(selection, !=, CLIPPOR_SELECTION_TYPE_NONE);

    *(uint *)data += 15;
    wayland_data_offer_destroy(offer);
}

static WaylandDataDeviceListener device_listener = {
    .data_offer = wayland_data_device_event_data_offer,
    .selection = wayland_data_device_event_selection,
    .finished = NULL
};

static void
wayland_data_source_event_send(
    void *data, WaylandDataSource *source, const char *mime_type, int32_t fd
)
{
    g_assert_nonnull(data);
    g_assert_nonnull(source);
    g_assert_cmpstr(mime_type, ==, "TEST");
    g_assert_cmpint(fd, >=, 0);

    *(uint *)data += 1;
    close(fd);
}

static void
wayland_data_source_event_cancelled(void *data, WaylandDataSource *source)
{
    g_assert_nonnull(data);
    g_assert_nonnull(source);
    *(uint *)data += 2;
}

static WaylandDataSourceListener source_listener = {
    .send = wayland_data_source_event_send,
    .cancelled = wayland_data_source_event_cancelled
};

/*
 * Test if Wayland events such as a new selection are received properly
 */
static void
test_wayland_connection_events(void)
{
    WaylandCompositor *wc = wayland_compositor_start();

    GError *error = NULL;
    g_autoptr(WaylandConnection) ct =
        wayland_connection_new(wc->display, &error);

    g_assert_no_error(error);
    g_assert_nonnull(ct);

    WaylandSeat *seat = wayland_connection_get_seat(ct, NULL);
    g_autoptr(WaylandDataDeviceManager) manager =
        wayland_connection_get_data_device_manager(ct);

    g_assert_nonnull(manager);

    // Test data device and data offer

    g_autoptr(WaylandDataDevice) device =
        wayland_data_device_manager_get_data_device(manager, seat);

    g_assert_nonnull(device);

    uint check = 0;
    wayland_data_device_add_listener(device, &device_listener, &check);

    wl_copy(wc, FALSE, NULL, "REGULAR");
    wl_copy(wc, TRUE, NULL, "PRIMARY");

    wayland_connection_roundtrip(ct, &error);
    g_assert_no_error(error);

    g_assert_cmpint(check, ==, 100);
    check = 0;

    // Test data source
    g_autoptr(WaylandDataSource) source =
        wayland_data_device_manager_create_data_source(manager);

    wayland_data_source_add_listener(source, &source_listener, &check);

    wayland_data_source_offer(source, "TEST");
    wayland_data_device_set_seletion(
        device, source, CLIPPOR_SELECTION_TYPE_REGULAR
    );

    wayland_connection_roundtrip(ct, &error);
    g_assert_no_error(error);

    char *cmdline[] = {"wl-paste", "-t", "TEST", NULL};
    char **environment =
        g_environ_setenv(g_get_environ(), "WAYLAND_DISPLAY", wc->display, TRUE);

    g_assert_true(g_spawn_async(
        NULL, cmdline, environment, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL,
        &error
    ));
    g_assert_no_error(error);

    wayland_connection_dispatch(ct, &error);
    g_assert_no_error(error);

    // Send event
    g_assert_cmpint(check, ==, 31);

    check = 0;

    // Cancelled event
    wl_copy(wc, FALSE, NULL, "REGULAR");

    wayland_connection_dispatch(ct, &error);
    g_assert_no_error(error);

    g_assert_cmpint(check, ==, 52);

    g_strfreev(environment);

    wayland_compositor_stop(wc);
    wayland_connection_free_static();
}

/*
 * Test if connection object unrefs itself when compositor is killed
 */
static void
test_wayland_connection_compositor_killed(void)
{
    WaylandCompositor *wc = wayland_compositor_start();
    g_autoptr(GMainContext) context = g_main_context_new();

    GError *error = NULL;
    WaylandConnection *ct = wayland_connection_new(wc->display, &error);

    g_assert_no_error(error);
    g_assert_nonnull(ct);

    GWeakRef ref;

    g_weak_ref_init(&ref, ct);

    wayland_connection_install_source(ct, context);

    while (g_main_context_pending(context))
        g_main_context_iteration(context, FALSE);

    wayland_compositor_stop(wc);

    int64_t start = g_get_monotonic_time();

    while (g_get_monotonic_time() - start < 3 * G_USEC_PER_SEC)
    {
        WaylandConnection *ct =g_weak_ref_get(&ref);

        if (ct == NULL)
            break;
        else 
            g_object_unref(ct);
        g_main_context_iteration(context, FALSE);
    }

    g_assert_null(g_weak_ref_get(&ref));

    g_weak_ref_clear(&ref);

    wayland_connection_free_static();
}

int
main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    g_test_init(&argc, &argv, NULL);

    pre_startup();

    struct sigaction sa;
    set_signal_handler(&sa);

    g_test_add_func(
        "/wayland/connection/startup", test_wayland_connection_startup
    );
    g_test_add_func(
        "/wayland/connection/get-seat", test_wayland_connection_get_seat
    );
    g_test_add_func(
        "/wayland/connection/data-device-events", test_wayland_connection_events
    );
    g_test_add_func(
        "/wayland/connection/compositor-killed",
        test_wayland_connection_compositor_killed
    );

    return g_test_run();
}
