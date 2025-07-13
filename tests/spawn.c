#include "spawn.h"
#include "server.h"
#include "util.h"
#include <glib-unix.h>
#include <glib.h>
#include <stdio.h>
#include <unistd.h>

static char *compositor_argv[] = {"labwc", "-c", "NONE", "-d", NULL};

static char *config_file = NULL;
static char *data_directory = NULL;

/*
 * Kill all children processes on SIGABRT & SIGTRAP and remove config file +
 * data directory
 */
static void
handle_signal(int signum G_GNUC_UNUSED)
{
    if (config_file != NULL)
        g_unlink(config_file);
    if (data_directory != NULL)
        util_remove_dir(data_directory, NULL);

    pid_t pgid = getpgrp();
    kill(-pgid, SIGKILL);
}

void
set_signal_handler(struct sigaction *sa)
{
    sa->sa_handler = handle_signal;
    sigemptyset(&sa->sa_mask);
    sa->sa_flags = 0;

    sigaction(SIGTRAP, sa, NULL);
    sigaction(SIGABRT, sa, NULL);
}

WaylandCompositor *
wayland_compositor_start(void)
{
    GError *error = NULL;
    WaylandCompositor *wc = g_new0(WaylandCompositor, 1);

    gboolean ret;
    int stderr_fd;
    FILE *out;
    char **environment = g_get_environ();

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

    return wc;
}

void
wayland_compositor_stop(WaylandCompositor *self)
{
    g_assert(self != NULL);

    kill(self->pid, SIGTERM);

    g_free(self->display);
    g_free(self);
}

/*
 * If "format" is NULL then the selection is cleared.
 */
void
wl_copy(WaylandCompositor *wc, gboolean primary, char *format, ...)
{
    char *primary_flag = primary ? "-p" : NULL;

    char *text = NULL;

    if (format != NULL)
    {
        va_list args;

        va_start(args, format);

        text = g_strdup_vprintf(format, args);

        va_end(args);
    }
    else
        text = g_strdup("-c");

    char *cmdline[] = {"wl-copy", text, primary_flag, NULL};
    GError *error = NULL;
    int status;

    char **environment =
        g_environ_setenv(g_get_environ(), "WAYLAND_DISPLAY", wc->display, TRUE);

    gboolean ret = g_spawn_sync(
        NULL, cmdline, environment,
        G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL |
            G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL, NULL, &status, &error
    );

    g_strfreev(environment);

    if (!ret)
        g_error("%s", error->message);
    g_assert(status == 0);

    g_free(text);
}

char *
wl_paste(
    WaylandCompositor *wc, gboolean primary, gboolean newline, char *mime_type
)
{
    int i = 1;
    char *cmdline[] = {"wl-paste", NULL, NULL, NULL, NULL, NULL};
    GError *error = NULL;
    char *output = NULL;
    int status;

    if (primary)
        cmdline[i++] = "-p";
    if (!newline)
        cmdline[i++] = "-n";
    if (mime_type != NULL)
    {
        cmdline[i++] = "-t";
        cmdline[i++] = mime_type;
    }

    char **environment =
        g_environ_setenv(g_get_environ(), "WAYLAND_DISPLAY", wc->display, TRUE);

    gboolean ret = g_spawn_sync(
        NULL, cmdline, environment,
        G_SPAWN_STDERR_TO_DEV_NULL | G_SPAWN_SEARCH_PATH, NULL, NULL, &output,
        NULL, &status, &error
    );

    g_strfreev(environment);

    if (!ret)
        g_error("%s", error->message);

    if (status != 0)
    {
        // Selection is empty
        g_free(output);
        return NULL;
    }

    return output;
}

static gpointer
server_thread(gpointer data)
{
    ServerInstance *server = data;

    gboolean ret = server_start(server->config_file, server->data_directory);

    server->stopped = TRUE;

    return GINT_TO_POINTER(ret);
}

ServerInstance *
run_server(const char *test_name, const char *config_contents)
{
    ServerInstance *server = g_new0(ServerInstance, 1);

    server->test_name = g_strdup(test_name);
    server->config_file = g_strdup_printf("%s.toml", test_name);
    server->data_directory = g_strdup_printf("%s_data", test_name);

    g_unlink(server->config_file);
    util_remove_dir(server->data_directory, NULL);

    config_file = server->config_file;
    data_directory = server->data_directory;

    // Write contents to config file
    GError *error = NULL;
    gboolean ret =
        g_file_set_contents(server->config_file, config_contents, -1, &error);

    if (!ret)
        g_error("%s", error->message);

    server->stopped = FALSE;
    server->thread = g_thread_new(test_name, server_thread, server);

    while (!server->stopped && !server_is_running())
        ;

    server->loop = server_get_main_loop();

    return server;
}

gboolean
stop_server(ServerInstance *server)
{
    g_main_loop_quit(server->loop);

    gboolean ret = GPOINTER_TO_INT(g_thread_join(server->thread));
    g_thread_unref(server->thread);

    g_unlink(server->config_file);
    util_remove_dir(server->data_directory, NULL);
    g_free(server->config_file);
    g_free(server->data_directory);
    g_free(server->test_name);
    g_free(server);

    config_file = NULL;
    data_directory = NULL;

    while(server_is_running());

    return ret;
}

gboolean
restart_server(ServerInstance *server)
{
    g_main_loop_quit(server->loop);

    gboolean ret = GPOINTER_TO_INT(g_thread_join(server->thread));
    g_thread_unref(server->thread);

    if (ret)
    {
        server->stopped = FALSE;
        server->thread = g_thread_new(server->test_name, server_thread, server);

        while (!server->stopped && !server_is_running())
            ;

        server->loop = server_get_main_loop();
    }

    return ret;
}
