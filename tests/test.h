#pragma once

#include <glib.h>

#define TEST_ARGS TestFixture *fixture, gconstpointer user_data G_GNUC_UNUSED
#define TEST_AARGS TestFixture *fixture, gconstpointer user_data
#define TEST_UARGS                                                             \
    TestFixture *fixture G_GNUC_UNUSED, gconstpointer user_data G_GNUC_UNUSED

#define TEST(path, func)                                                       \
    g_test_add(                                                                \
        path, TestFixture, NULL, test_fixture_setup, func,                     \
        test_fixture_teardown                                                  \
    );

#define PTRARRAY_HAS_STR(arr, str)                                             \
    g_ptr_array_find_with_equal_func(arr, str, g_str_equal, NULL)

typedef struct
{
    char *display;
    GPid pid;
} WaylandCompositor;

void test_setup(void);

void main_context_dispatch(GMainContext *context);
void main_context_run(GMainContext *context);
void main_context_stop(void);

WaylandCompositor *wayland_compositor_new(void);
void wayland_compositor_destroy(WaylandCompositor *self);

void wl_copy(
    WaylandCompositor *wc, gboolean primary, const char *text,
    const char *mime_type
);
const char *
wl_paste(WaylandCompositor *wc, gboolean primary, const char *mime_type);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaylandCompositor, wayland_compositor_destroy)
