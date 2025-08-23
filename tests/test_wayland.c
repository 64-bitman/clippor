#include "test.h"
#include "wayland-connection.h"
#include "wayland-seat.h"
#include "wayland-selection.h"
#include <glib.h>
#include <locale.h>

typedef enum
{
    GOT_NONE = 0,
    GOT_OFFER = 1 << 0,
    GOT_DATA_OFFER = 1 << 1,
    GOT_DATA_SELECTION_REGULAR = 1 << 2,
    GOT_DATA_SELECTION_PRIMARY = 1 << 3,
    GOT_DATA_SEND = 1 << 4,
    GOT_DATA_CANCELLED = 1 << 5
} TestFlags;

typedef struct
{
    GMainContext *context;
    WaylandCompositor *wc;
} TestFixture;

static void
test_fixture_setup(TEST_ARGS)
{
    fixture->context = g_main_context_new();
    fixture->wc = wayland_compositor_new();

    g_main_context_push_thread_default(fixture->context);
}

static void
test_fixture_teardown(TEST_ARGS)
{
    wayland_compositor_destroy(fixture->wc);

    g_main_context_pop_thread_default(fixture->context);
    g_main_context_unref(fixture->context);
}

static gboolean
data_offer_listener_event_offer(
    void *data G_GNUC_UNUSED, WaylandDataOffer *offer G_GNUC_UNUSED,
    const char *mime_type G_GNUC_UNUSED
)
{
    uint *bitfield = data;

    *bitfield |= GOT_OFFER;
    return TRUE;
}

static const WaylandDataOfferListener data_offer_listener = {
    .offer = data_offer_listener_event_offer
};

static void
data_device_listener_event_data_offer(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED, WaylandDataOffer *offer
)
{
    uint *bitfield = data;

    *bitfield |= GOT_DATA_OFFER;

    wayland_data_offer_add_listener(offer, &data_offer_listener, data);
}

/*
 * Don't do anything, let the individual selections do the stuff.
 */
static void
data_device_listener_event_selection(
    void *data, WaylandDataDevice *device G_GNUC_UNUSED,
    WaylandDataOffer *offer, ClipporSelectionType selection
)
{
    uint *bitfield = data;

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        *bitfield |= GOT_DATA_SELECTION_REGULAR;
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        *bitfield |= GOT_DATA_SELECTION_PRIMARY;

    if (offer != NULL)
        wayland_data_offer_destroy(offer);
}

static void
data_device_listener_event_finished(
    void *data G_GNUC_UNUSED, WaylandDataDevice *device G_GNUC_UNUSED
)
{
}

static const WaylandDataDeviceListener data_device_listener = {
    .data_offer = data_device_listener_event_data_offer,
    .selection = data_device_listener_event_selection,
    .finished = data_device_listener_event_finished
};

static void
data_source_listener_event_send(
    void *data, WaylandDataSource *source G_GNUC_UNUSED,
    const char *mime_type G_GNUC_UNUSED, int fd
)
{
    uint *bitfield = data;

    *bitfield |= GOT_DATA_SEND;

    write(fd, "test", 4);
    close(fd);
}

static void
data_source_listener_event_cancelled(void *data, WaylandDataSource *source)
{
    uint *bitfield = data;

    *bitfield |= GOT_DATA_CANCELLED;
    wayland_data_source_destroy(source);
}

static const WaylandDataSourceListener data_source_listener = {
    .send = data_source_listener_event_send,
    .cancelled = data_source_listener_event_cancelled
};

/*
 * Test if Wayland events from connection are handled correctly
 */
static void
test_wayland_connection_events(TEST_ARGS)
{
    WaylandCompositor *wc = fixture->wc;
    g_autoptr(GError) error = NULL;
    g_autoptr(WaylandConnection) ct = wayland_connection_new(wc->display);

    g_assert_nonnull(ct);

    wayland_connection_start(ct, &error);
    g_assert_no_error(error);
    wayland_connection_install_source(ct, fixture->context);

    // Clear up the event queue
    main_context_dispatch(fixture->context);

    g_autoptr(WaylandSeat) seat = wayland_connection_get_seat(ct, NULL);

    g_assert_nonnull(seat);

    g_autoptr(WaylandDataDeviceManager) manager =
        wayland_connection_get_data_device_manager(ct, NULL);

    g_assert_nonnull(manager);

    g_autoptr(WaylandDataDevice) device =
        wayland_data_device_manager_get_data_device(manager, seat);

    g_assert_nonnull(device);

    uint bitfield = GOT_NONE;

    // Test data device and data offer events
    wayland_data_device_add_listener(device, &data_device_listener, &bitfield);

    wl_copy(wc, FALSE, "hello", "text/plain");
    main_context_dispatch(fixture->context);

    g_assert_true(bitfield & GOT_DATA_OFFER);
    g_assert_true(bitfield & GOT_DATA_SELECTION_REGULAR);
    g_assert_true(bitfield & GOT_OFFER);

    wl_copy(wc, TRUE, "hello", "text/plain");
    main_context_dispatch(fixture->context);

    g_assert_true(bitfield & GOT_DATA_SELECTION_PRIMARY);

    WaylandDataSource *source =
        wayland_data_device_manager_create_data_source(manager);

    wayland_data_source_add_listener(source, &data_source_listener, &bitfield);

    wayland_data_source_offer(source, "text/plain");

    wayland_data_device_set_selection(
        device, source, CLIPPOR_SELECTION_TYPE_REGULAR
    );

    main_context_run(fixture->context);

    g_assert_cmpstr(wl_paste(wc, FALSE, "text/plain"), ==, "test");
    g_assert_true(bitfield & GOT_DATA_SEND);

    main_context_stop();

    // Make source get cancelled event
    wl_copy(wc, FALSE, "hello", "text/plain");

    main_context_dispatch(fixture->context);

    g_assert_true(bitfield & GOT_DATA_CANCELLED);
}

/*
 * Test if Wayland connection object becomes inert when connection is lost to
 * compositor.
 */
static void
test_wayland_connection_lost(TEST_ARGS)
{
    WaylandCompositor *wc = fixture->wc;
    g_autoptr(WaylandConnection) ct = wayland_connection_new(wc->display);
    g_autoptr(GError) error = NULL;

    wayland_connection_start(ct, &error);
    g_assert_no_error(error);
    wayland_connection_install_source(ct, fixture->context);

    g_assert_true(wayland_connection_is_active(ct));

    main_context_dispatch(fixture->context);

    g_clear_pointer(&fixture->wc, wayland_compositor_destroy);

    main_context_dispatch(fixture->context);

    g_assert_false(wayland_connection_is_active(ct));
}

static void
selection_update(ClipporSelection *sel G_GNUC_UNUSED, gboolean *got)
{
    *got = TRUE;
}

/*
 * Test if the Wayland selection object updates on new selections.
 */
static void
test_wayland_selection_update(TEST_ARGS)
{
    WaylandCompositor *wc = fixture->wc;
    g_autoptr(WaylandConnection) ct = wayland_connection_new(wc->display);
    g_autoptr(GError) error = NULL;

    wayland_connection_start(ct, &error);
    g_assert_no_error(error);
    wayland_connection_install_source(ct, fixture->context);

    main_context_dispatch(fixture->context);

    g_autoptr(WaylandSeat) seat = wayland_connection_get_seat(ct, NULL);
    g_assert_nonnull(seat);

    g_autoptr(WaylandSelection) wsel =
        wayland_seat_get_selection(seat, CLIPPOR_SELECTION_TYPE_REGULAR);
    ClipporSelection *sel = CLIPPOR_SELECTION(wsel);
    gboolean got = FALSE;

    g_signal_connect(sel, "update", G_CALLBACK(selection_update), &got);

    wl_copy(wc, FALSE, "test", NULL);

    main_context_dispatch(fixture->context);

    g_assert_true(got);

    g_autoptr(GPtrArray) mime_types = clippor_selection_get_mime_types(sel);

    g_assert_cmpuint(mime_types->len, ==, 5);

    g_assert_true(PTRARRAY_HAS_STR(mime_types, "TEXT"));
    g_assert_true(PTRARRAY_HAS_STR(mime_types, "STRING"));
    g_assert_true(PTRARRAY_HAS_STR(mime_types, "UTF8_STRING"));
    g_assert_true(PTRARRAY_HAS_STR(mime_types, "text/plain"));
    g_assert_true(PTRARRAY_HAS_STR(mime_types, "text/plain;charset=utf-8"));

    g_autoptr(GInputStream) stream =
        clippor_selection_get_data(sel, "text/plain", &error);
    char buf[128] = {0};

    g_assert_no_error(error);
    g_input_stream_read(stream, buf, 127, NULL, &error);
    g_assert_no_error(error);

    g_assert_cmpstr(buf, ==, "test");
}

/*
 * Test if Wayland selection can be updated with a new entry.
 */
static void
test_wayland_selection_set(TEST_ARGS)
{
    WaylandCompositor *wc = fixture->wc;
    g_autoptr(WaylandConnection) ct = wayland_connection_new(wc->display);
    g_autoptr(GError) error = NULL;

    wayland_connection_start(ct, &error);
    g_assert_no_error(error);
    wayland_connection_install_source(ct, fixture->context);

    main_context_dispatch(fixture->context);

    g_autoptr(WaylandSeat) seat = wayland_connection_get_seat(ct, NULL);
    g_assert_nonnull(seat);

    g_autoptr(WaylandSelection) wsel =
        wayland_seat_get_selection(seat, CLIPPOR_SELECTION_TYPE_REGULAR);
    ClipporSelection *sel = CLIPPOR_SELECTION(wsel);

    g_autoptr(ClipporEntry) entry = clippor_entry_new(NULL);
    g_autoptr(GBytes) bytes = g_string_free_to_bytes(g_string_new("test"));
    g_autoptr(GBytes) bytes2 = g_string_free_to_bytes(g_string_new("test2"));

    clippor_entry_add_mime_type(entry, "text/plain", bytes);
    clippor_entry_add_mime_type(entry, "TEXT", bytes);
    clippor_entry_add_mime_type(entry, "text/html", bytes2);

    clippor_selection_update(sel, entry, FALSE, &error);
    g_assert_no_error(error);

    main_context_run(fixture->context);

    g_assert_cmpstr(wl_paste(wc, FALSE, "text/plain"), ==, "test");
    g_assert_cmpstr(wl_paste(wc, FALSE, "TEXT"), ==, "test");
    g_assert_cmpstr(wl_paste(wc, FALSE, "text/html"), ==, "test2");

    main_context_stop();

    g_assert_true(clippor_selection_is_owned(sel));

    wl_copy(wc, FALSE, "test", NULL);

    main_context_dispatch(fixture->context);

    g_assert_false(clippor_selection_is_owned(sel));
}

/*
 * Test behaviour when Wayland selection object becomes inert.
 */
static void
test_wayland_selection_inert(TEST_ARGS)
{
    WaylandCompositor *wc = fixture->wc;
    g_autoptr(WaylandConnection) ct = wayland_connection_new(wc->display);
    g_autoptr(GError) error = NULL;

    wayland_connection_start(ct, &error);
    g_assert_no_error(error);
    wayland_connection_install_source(ct, fixture->context);

    g_assert_true(wayland_connection_is_active(ct));

    g_autoptr(WaylandSeat) seat = wayland_connection_get_seat(ct, NULL);
    g_assert_nonnull(seat);

    g_autoptr(WaylandSelection) wsel =
        wayland_seat_get_selection(seat, CLIPPOR_SELECTION_TYPE_REGULAR);
    ClipporSelection *sel = CLIPPOR_SELECTION(wsel);

    main_context_dispatch(fixture->context);
    g_assert_false(clippor_selection_is_inert(sel));

    g_clear_pointer(&fixture->wc, wayland_compositor_destroy);
    main_context_dispatch(fixture->context);
    g_assert_true(clippor_selection_is_inert(sel));

    // Check if methods fail
    clippor_selection_update(sel, NULL, FALSE, &error);
    g_assert_error(
        error, CLIPPOR_SELECTION_ERROR, CLIPPOR_SELECTION_ERROR_INERT
    );
    g_clear_error(&error);

    clippor_selection_get_data(sel, "text/plain", &error);
    g_assert_error(
        error, CLIPPOR_SELECTION_ERROR, CLIPPOR_SELECTION_ERROR_INERT
    );

    g_assert_null(clippor_selection_get_mime_types(sel));
    g_assert_false(clippor_selection_is_owned(sel));
}

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    g_test_init(&argc, &argv, NULL);

    test_setup();

    TEST("/wayland/connection/events", test_wayland_connection_events);
    TEST("/wayland/connection/lost", test_wayland_connection_lost);
    TEST("/wayland/selection/update", test_wayland_selection_update);
    TEST("/wayland/selection/set", test_wayland_selection_set);
    TEST("/wayland/selection/inert", test_wayland_selection_inert);

    return g_test_run();
}
