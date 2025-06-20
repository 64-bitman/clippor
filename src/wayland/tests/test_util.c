#include "test_util.h"
#include <glib.h>
#include <stdio.h>
#include <unistd.h>

static gchar *compositor_argv[] = {"labwc", "-c", "NONE", "-d", NULL};

WaylandCompositor *
wayland_compositor_new(gboolean set_env)
{
    GError *error = NULL;

    WaylandCompositor *wc = g_new0(WaylandCompositor, 1);
    gboolean ret;
    gint stderr_fd;
    FILE *out;

    gchar **environment = g_get_environ();

    environment =
        g_environ_setenv(environment, "WLR_BACKENDS", "headless", TRUE);

    ret = g_spawn_async_with_pipes(
        NULL, compositor_argv, environment,
        G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDIN_FROM_DEV_NULL |
            G_SPAWN_SEARCH_PATH,
        NULL, NULL, &wc->pid, NULL, NULL, &stderr_fd, &error
    );

    if (!ret)
        g_error("%s", error->message);

    out = fdopen(stderr_fd, "r");

    if (out == NULL)
        g_error("fdopen() returned NULL");

    // Find what display it is using
    size_t sz;
    char *buf = NULL;

    const char *pattern = "(?<=WAYLAND_DISPLAY=).+$";
    GRegex *regex =
        g_regex_new(pattern, G_REGEX_OPTIMIZE, G_REGEX_MATCH_DEFAULT, &error);

    if (regex == NULL)
        g_error("%s", error->message);

    while (errno = 0, getline(&buf, &sz, out) != -1)
    {
        GMatchInfo *match = NULL;
        g_regex_match(regex, buf, G_REGEX_MATCH_DEFAULT, &match);

        if (g_match_info_matches(match))
        {
            wc->display = g_match_info_fetch(match, 0);
            g_match_info_free(match);
            break;
        }
        g_match_info_free(match);
    }

    g_regex_unref(regex);
    g_free(buf);
    g_strfreev(environment);

    if (errno != 0)
        g_error("%s", g_strerror(errno));

    if (set_env)
        wayland_compositor_set_env(wc);

    return wc;
}

void
wayland_compositor_destroy(WaylandCompositor *self)
{
    g_assert(self != NULL);

    kill(self->pid, SIGTERM);

    wayland_compositor_restore_env(self);

    g_free(self->display);
    g_free(self);
}

void
wayland_compositor_set_env(WaylandCompositor *self)
{
    g_assert(self != NULL && self->display != NULL);

    self->restore_display = g_strdup(g_getenv("WAYLAND_DISPLAY"));

    g_setenv("WAYLAND_DISPLAY", self->display, TRUE);
}

void
wayland_compositor_restore_env(WaylandCompositor *self)
{
    if (self->restore_display == NULL)
        return;
    g_setenv("WAYLAND_DISPLAY", self->restore_display, TRUE);
    g_free(self->restore_display);
    self->restore_display = NULL;
}

void
wl_copy(gboolean primary, gchar *format, ...)
{
    gchar *primary_flag = primary ? "-p" : NULL;

    va_list args;

    va_start(args, format);

    gchar *text = g_strdup_vprintf(format, args);

    va_end(args);

    gchar *cmdline[] = {"wl-copy", text, primary_flag, NULL};
    GError *error = NULL;
    gint status;

    gboolean ret = g_spawn_sync(
        NULL, cmdline, NULL,
        G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL |
            G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL, NULL, &status, &error
    );

    if (!ret)
        g_error("%s", error->message);
    g_assert(status == 0);

    g_free(text);
}

gchar *
wl_paste(gboolean primary, gboolean newline, gchar *mime_type)
{
    gint i = 1;
    gchar *cmdline[] = {"wl-paste", NULL, NULL, NULL, NULL, NULL};
    GError *error = NULL;
    gchar *output = NULL;
    gint status;

    if (primary)
        cmdline[i++] = "-p";
    if (!newline)
        cmdline[i++] = "-n";
    if (mime_type != NULL)
    {
        cmdline[i++] = "-t";
        cmdline[i++] = mime_type;
    }

    gboolean ret = g_spawn_sync(
        NULL, cmdline, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &output, NULL,
        &status, &error
    );

    if (!ret)
        g_error("%s", error->message);

    if (status != 0)
    {
        g_free(output);
        return NULL;
    }

    return output;
}

/*
 * Kill all children processes on SIGABRT
 */
static void
handle_sigabrt(int signum G_GNUC_UNUSED)
{
    pid_t pgid = getpgrp();
    kill(-pgid, SIGKILL);

    _exit(1);
}

void
set_sigabrt_handler(struct sigaction *sa)
{
    sa->sa_handler = handle_sigabrt;
    sigemptyset(&sa->sa_mask);
    sa->sa_flags = 0;

    sigaction(SIGABRT, sa, NULL);
}
