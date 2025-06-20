#include "wayland-seat.h"
#include "test_util.h"
#include "wayland-connection.h"
#include <glib.h>
#include <locale.h>
#include <wayland-client.h>

typedef struct
{
    WaylandCompositor *wc;
    WaylandConnection *ct;
    WaylandSeat *seat;
    GMainLoop *loop;
} WaylandSeatFixture;

static void
wayland_seat_fixture_set_up(
    WaylandSeatFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
)
{
    fixture->wc = wayland_compositor_new(TRUE);
    fixture->ct = wayland_connection_new(fixture->wc->display, NULL);
    fixture->seat = wayland_connection_get_seat(fixture->ct, NULL);

    g_assert(fixture->ct != NULL);
}

static void
wayland_seat_fixture_tear_down(
    WaylandSeatFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
)
{
    if (fixture->wc != NULL)
    {
        g_object_unref(fixture->ct);
        wayland_compositor_destroy(fixture->wc);
    }
}

static void
test_wayland_seat_new(
    WaylandSeatFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
)
{
    WaylandSeat *seat = fixture->seat;

    g_assert_cmpstr(wayland_seat_get_name(seat), ==, "seat0");
    g_assert_cmpuint(wayland_seat_get_numerical_name(seat), >, 0);
    g_assert_true(wayland_seat_get_proxy(seat) != NULL);
}

static gpointer
test_wayland_seat_clipboard_thread(gpointer data)
{
    WaylandSeatFixture *fixture = data;

    g_main_loop_run(fixture->loop);

    return NULL;
}

static void
test_wayland_seat_clipboard(
    WaylandSeatFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
)
{
    WaylandSeat *seat = fixture->seat;
    GThread *thread;

    wayland_connection_install_source(fixture->ct, NULL);

    fixture->loop = g_main_loop_new(NULL, FALSE);
    thread = g_thread_new("loop", test_wayland_seat_clipboard_thread, fixture);

    // Check if seat is unreferenced when connection is lost
    GWeakRef ref;

    g_weak_ref_init(&ref, seat);

    wayland_compositor_destroy(fixture->wc);
    fixture->wc = NULL;

    g_usleep(100 * 1000);
    g_assert_true(g_weak_ref_get(&ref) == NULL);

    g_main_loop_quit(fixture->loop);
    g_main_loop_unref(fixture->loop);

    g_thread_join(thread);
    g_thread_unref(thread);
}

int
main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    struct sigaction sa;
    set_sigabrt_handler(&sa);

    g_test_init(&argc, &argv, NULL);

    g_test_add(
        "/seat/new", WaylandSeatFixture, NULL, wayland_seat_fixture_set_up,
        test_wayland_seat_new, wayland_seat_fixture_tear_down
    );
    g_test_add(
        "/seat/clipboard", WaylandSeatFixture, NULL,
        wayland_seat_fixture_set_up, test_wayland_seat_clipboard,
        wayland_seat_fixture_tear_down
    );

    return g_test_run();
}
