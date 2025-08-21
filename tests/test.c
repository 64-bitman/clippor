#include "test.h"
#include <glib.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

GPtrArray *PIDS;
struct sigaction SA;

/*
 * Kill all child processes on SIGABRT or SIGTRAP. This prevents meson from
 * hanging when an assertion fails, because there are still child processes
 * running.
 */
static void
signal_handler(int sig G_GNUC_UNUSED)
{
    for (uint i = 0; i < PIDS->len; i++)
    {
        kill(*(GPid *)PIDS->pdata[i], SIGTERM);
        waitpid(*(GPid *)PIDS->pdata[i], NULL, 0);
    }

    SA.sa_handler = SIG_DFL;
    sigaction(sig, &SA, NULL);

    raise(sig);
}

/*
 * Stuff to do before running tests
 */
void
test_setup(void)
{
    PIDS = g_ptr_array_new();

    SA.sa_handler = signal_handler;

    sigemptyset(&SA.sa_mask);
    SA.sa_flags = 0;
    sigaction(SIGABRT, &SA, NULL);
    sigaction(SIGTRAP, &SA, NULL);
}

/*
 * Dispatch queued up events in the given context
 */
void
main_context_dispatch(GMainContext *context)
{
    while (g_main_context_pending(context))
        g_main_context_iteration(context, TRUE);
}

WaylandCompositor *
wayland_compositor_new(void)
{
    WaylandCompositor *wc = g_new0(WaylandCompositor, 1);

    GError *error = NULL;
    char *argv[] = {"labwc", "-c", "NONE", "-d", NULL};
    char **envp =
        g_environ_setenv(g_get_environ(), "WLR_BACKENDS", "headless", TRUE);
    int errfd;

    g_spawn_async_with_pipes(
        NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, &wc->pid, NULL, NULL,
        &errfd, &error
    );

    g_assert_no_error(error);

    FILE *stream = fdopen(errfd, "r");

    g_assert_nonnull(stream);

    size_t sz;
    char *line = NULL;
    g_autoptr(GRegex) regex = g_regex_new(
        "(?<=WAYLAND_DISPLAY=).*$", G_REGEX_OPTIMIZE, G_REGEX_MATCH_DEFAULT,
        &error
    );
    GMatchInfo *match;

    g_assert_no_error(error);

    while (getline(&line, &sz, stream) != -1)
    {
        if (g_regex_match(regex, line, G_REGEX_MATCH_DEFAULT, &match))
        {
            wc->display = g_match_info_fetch(match, 0);
            g_match_info_free(match);
            break;
        }
        g_match_info_free(match);
    }

    g_free(line);
    fclose(stream);
    close(errfd);

    g_assert_nonnull(wc->display);

    g_strfreev(envp);
    g_ptr_array_add(PIDS, &wc->pid);

    return wc;
}

void
wayland_compositor_destroy(WaylandCompositor *self)
{
    g_ptr_array_remove(PIDS, &self->pid);
    kill(self->pid, SIGTERM);

    g_free(self->display);
    g_free(self);
}
