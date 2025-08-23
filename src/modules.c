#include "modules.h"
#include "config.h"
#include <glib-unix.h>
#include <glib.h>
#include <gmodule.h>

static GModule *MODULE_WAYLAND;

WaylandModule WAYLAND_FUNCS;

#ifndef WAYLAND_LINKED
static void
modules_try_wayland(const char *dir)
{
    if (MODULE_WAYLAND != NULL)
        return;

    g_autofree char *path =
        g_build_path("/", dir, "libclippor-wayland.so", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS))
        return;

    MODULE_WAYLAND = g_module_open(path, G_MODULE_BIND_LAZY);

    if (MODULE_WAYLAND == NULL)
    {
        g_debug("Failed opening Wayland module: %s", g_module_error());
        return;
    }

    void *funcs[][2] = {
        {"wayland_connection_new", &WAYLAND_FUNCS.connection_new},
        {"wayland_connection_start", &WAYLAND_FUNCS.connection_start},
        {"wayland_connection_stop", &WAYLAND_FUNCS.connection_stop},
        {"wayland_connection_install_source",
         &WAYLAND_FUNCS.connection_install_source},
        {"wayland_connection_get_seat", &WAYLAND_FUNCS.connection_get_seat},
        {"wayland_seat_get_selection", &WAYLAND_FUNCS.seat_get_selection},
    };

    for (uint i = 0; i < G_N_ELEMENTS(funcs); i++)
    {
        if (!g_module_symbol(MODULE_WAYLAND, funcs[i][0], funcs[i][1]))
            goto fail;
    }

    g_debug("Opened module wayland at %s", path);
    WAYLAND_FUNCS.available = TRUE;

    return;
fail:
    g_clear_pointer(&MODULE_WAYLAND, g_module_close);
    g_debug("Failed reading symbols from Wayland module: %s", g_module_error());
}
#endif // WAYLAND_LINKED

/*
 * Find available modules and open them. Should only be called once during
 * program lifetime.
 */
void
modules_init(void)
{
    const char *paths[] = {
        g_getenv("CLIPPOR_MODULES_PATH"), "/usr/local/share/clippor",
        "/usr/share/clippor"
    };

    for (uint i = 0; i < G_N_ELEMENTS(paths); i++)
    {
        const char *path = paths[i];

        if (path == NULL)
            continue;

#ifndef WAYLAND_LINKED
        modules_try_wayland(path);
#endif
    }

#ifdef WAYLAND_LINKED
    WAYLAND_FUNCS.connection_new = wayland_connection_new;
    WAYLAND_FUNCS.connection_start = wayland_connection_start;
    WAYLAND_FUNCS.connection_stop = wayland_connection_stop;
    WAYLAND_FUNCS.connection_install_source = wayland_connection_install_source;
    WAYLAND_FUNCS.connection_get_seat = wayland_connection_get_seat;
    WAYLAND_FUNCS.seat_get_selection = wayland_seat_get_selection;
    WAYLAND_FUNCS.available = TRUE;
#endif

    if (!WAYLAND_FUNCS.available)
        g_debug("Wayland module not found");
}

void
modules_uninit(void)
{
    if (MODULE_WAYLAND != NULL)
        g_module_close(MODULE_WAYLAND);
}
