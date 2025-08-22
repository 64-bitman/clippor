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
    PIDS = g_ptr_array_new_with_free_func(g_free);

    SA.sa_handler = signal_handler;

    sigemptyset(&SA.sa_mask);
    SA.sa_flags = 0;
    sigaction(SIGABRT, &SA, NULL);
    sigaction(SIGTRAP, &SA, NULL);
}

static void
add_pid(GPid pid)
{
    GPid *mem = g_malloc(sizeof(pid));

    *mem = pid;
    g_ptr_array_add(PIDS, mem);
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

static GThread *THREAD;
static GMainLoop *LOOP;
static GMainContext *CONTEXT;

static void *
context_thread_func(void *data G_GNUC_UNUSED)
{
    g_main_context_push_thread_default(CONTEXT);

    g_main_loop_run(LOOP);
    g_main_loop_unref(LOOP);

    g_main_context_pop_thread_default(CONTEXT);

    return NULL;
}

/*
 * Run context in a separate thread
 */
void
main_context_run(GMainContext *context)
{
    main_context_dispatch(context);

    g_main_context_pop_thread_default(context);

    CONTEXT = context;
    LOOP = g_main_loop_new(context, FALSE);
    THREAD = g_thread_new("test", context_thread_func, NULL);
}

static gboolean
main_context_invoke_func(void *data G_GNUC_UNUSED)
{
    g_main_loop_quit(LOOP);
    return G_SOURCE_REMOVE;
}

/*
 * Stop running the context in the different thread.
 */
void
main_context_stop(void)
{
    g_main_context_invoke(CONTEXT, main_context_invoke_func, NULL);

    g_thread_join(THREAD);
    g_main_context_push_thread_default(CONTEXT);
}

WaylandCompositor *
wayland_compositor_new(void)
{
    WaylandCompositor *wc = g_new0(WaylandCompositor, 1);

    g_autoptr(GError) error = NULL;
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

    // Find the log message that shows which display it is using
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

    add_pid(wc->pid);

    return wc;
}

void
wayland_compositor_destroy(WaylandCompositor *self)
{
    if (self == NULL)
        return;
    g_ptr_array_remove(PIDS, &self->pid);

    // Wait for process to exit
    while (errno = 0, kill(self->pid, SIGTERM), errno != ESRCH)
        ;

    g_free(self->display);
    g_free(self);
}

void
wl_copy(
    WaylandCompositor *wc, gboolean primary, const char *text,
    const char *mime_type
)
{
    uint i = 0;
    char *argv[10] = {NULL};
    char **envp =
        g_environ_setenv(g_get_environ(), "WAYLAND_DISPLAY", wc->display, TRUE);
    g_autoptr(GError) error = NULL;

    argv[i++] = "wl-copy";
    if (primary)
        argv[i++] = "-p";
    if (mime_type != NULL)
    {
        argv[i++] = "-t";
        argv[i++] = (char *)mime_type;
    }
    if (text == NULL)
        argv[i++] = "-c";
    else
        argv[i++] = (char *)text;

    g_spawn_sync(
        NULL, argv, envp,
        G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
            G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, NULL, NULL, NULL, &error
    );
    g_assert_no_error(error);

    g_strfreev(envp);
}

// Used to store the most recent wl_paste operation. This is so we don't have to
// free the memory returned by wl_paste.
static char *CUR_PASTE = NULL;

const char *
wl_paste(WaylandCompositor *wc, gboolean primary, const char *mime_type)
{
    uint i = 0;
    char *argv[10] = {NULL};
    char **envp =
        g_environ_setenv(g_get_environ(), "WAYLAND_DISPLAY", wc->display, TRUE);
    g_autoptr(GError) error = NULL;
    char *err;

    argv[i++] = "wl-paste";
    argv[i++] = "-n";
    if (primary)
        argv[i++] = "-p";
    if (mime_type != NULL)
    {
        argv[i++] = "-t";
        argv[i++] = (char *)mime_type;
    }

    g_free(CUR_PASTE);
    g_spawn_sync(
        NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, &CUR_PASTE, &err,
        NULL, &error
    );
    g_assert_no_error(error);

    if (err != NULL && *err != 0)
        g_warning("wl_paste failed: %s", err);
    g_free(err);
    g_strfreev(envp);

    return CUR_PASTE;
}
