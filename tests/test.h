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

typedef struct
{
    char *display;
    GPid pid;
} WaylandCompositor;

void test_setup(void);

void main_context_dispatch(GMainContext *context);

WaylandCompositor *wayland_compositor_new(void);
void wayland_compositor_destroy(WaylandCompositor *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(WaylandCompositor, wayland_compositor_destroy)
