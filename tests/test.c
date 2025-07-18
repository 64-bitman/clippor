#include "test.h"
#include "clippor-clipboard.h"
#include "clippor-entry.h"
#include "server.h"
#include <glib-unix.h>
#include <glib.h>
#include <stdio.h>
#include <unistd.h>

/*
 * Kill all children processes on SIGABRT and SIGTRAP
 */
static void
handle_signal(int signum G_GNUC_UNUSED)
{
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

void
pre_startup(void)
{
#ifndef DEBUG
    g_error("Tests must be run using a debug build");
#endif
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

    char *cmdline[] = {"labwc", "-c", "NONE", "-d", NULL};

    environment =
        g_environ_setenv(environment, "WLR_BACKENDS", "headless", TRUE);
    ret = g_spawn_async_with_pipes(
        NULL, cmdline, environment,
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
    g_assert_nonnull(wc->display);

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
wl_copy(
    WaylandCompositor *wc, gboolean primary, const char *mime_type,
    char *format, ...
)
{
    char *cmdline[] = {"wl-copy", NULL, NULL, NULL, NULL, NULL};
    int i = 1;
    g_autofree char *text;

    if (primary)
        cmdline[i++] = "-p";

    if (format != NULL)
    {
        va_list args;

        va_start(args, format);

        cmdline[i++] = text = g_strdup_vprintf(format, args);

        va_end(args);
    }
    else
        cmdline[i++] = text = g_strdup("-c");

    GError *error = NULL;
    int status;

    if (mime_type != NULL)
    {
        cmdline[i++] = "-t";
        cmdline[i++] = (char *)mime_type;
    }

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
}

char *
wl_paste(
    WaylandCompositor *wc, gboolean primary, gboolean newline,
    const char *mime_type
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
        cmdline[i++] = (char *)mime_type;
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

/*
 * Return allocated string of entry data for mime type
 */
char *
get_entry_contents(ClipporEntry *entry, const char *mime_type)
{

    GError *error = NULL;
    g_autoptr(ClipporData) data = NULL;

    g_assert_no_error(error);
    data = clippor_entry_get_data(entry, mime_type, &error);
    g_assert_no_error(error);

    size_t sz;
    const char *raw;

    raw = clippor_data_get_data(data, &sz);

    return g_strdup_printf("%.*s", (int)sz, raw);
}

/*
 * Compare if two entries are the same
 */
void
cmp_entry(ClipporEntry *entry, ClipporEntry *entry2)
{
    g_assert_cmpstr(
        clippor_entry_get_id(entry2), ==, clippor_entry_get_id(entry)
    );
    g_assert_cmpint(
        clippor_entry_get_creation_time(entry2), ==,
        clippor_entry_get_creation_time(entry)
    );
    g_assert_cmpint(
        clippor_entry_get_last_used_time(entry2), ==,
        clippor_entry_get_last_used_time(entry)
    );
    g_assert_cmpint(
        clippor_entry_is_starred(entry2), ==, clippor_entry_is_starred(entry)
    );

    GHashTable *d_mime_types = clippor_entry_get_mime_types(entry2);
    GHashTable *mime_types = clippor_entry_get_mime_types(entry);

    g_assert_cmpint(
        g_hash_table_size(d_mime_types), ==, g_hash_table_size(mime_types)
    );

    GHashTableIter iter;
    const char *mime_type;

    g_hash_table_iter_init(&iter, d_mime_types);

    while (g_hash_table_iter_next(&iter, (gpointer *)&mime_type, NULL))
        g_assert_true(g_hash_table_contains(mime_types, mime_type));

    g_autofree char *d_str = NULL, *str = NULL;

    d_str = get_entry_contents(entry, "text/plain");
    str = get_entry_contents(entry2, "text/plain");

    g_assert_cmpstr(d_str, ==, str);
}

static GMainContext *CONTEXT;
static GMainLoop *LOOP;
static GThread *LOOP_THREAD;

static char *CONFIG_CONTENTS;

static gboolean RUNNING = FALSE;
static gboolean THREAD_RUNNING = FALSE;

void
server_instance_start(const char *config_contents)
{
    CONTEXT = g_main_context_new();
    CONFIG_CONTENTS = g_strdup(config_contents);

    g_main_context_push_thread_default(CONTEXT);
    g_assert_true(server_start(
        CONFIG_CONTENTS, NULL, SERVER_FLAG_DB_IN_MEMORY | SERVER_FLAG_MANUAL
    ));

    RUNNING = TRUE;
}

/*
 * Dispatch all pending events
 */
void
server_instance_dispatch(void)
{
    g_assert(RUNNING);
    while (g_main_context_pending(CONTEXT))
        g_main_context_iteration(CONTEXT, TRUE);
}

static void *
server_thread(gpointer user_data G_GNUC_UNUSED)
{
    g_main_context_push_thread_default(CONTEXT);

    LOOP = g_main_loop_new(CONTEXT, FALSE);

    g_main_loop_run(LOOP);

    g_main_loop_unref(LOOP);
    g_main_context_pop_thread_default(CONTEXT);
    return NULL;
}

static gboolean
server_thread_quit(gpointer user_data G_GNUC_UNUSED)
{
    g_assert(g_main_context_is_owner(CONTEXT));
    g_main_loop_quit(LOOP);
    return G_SOURCE_REMOVE;
}

/*
 * Runs main loop in a separate thread until server_instance_pause() is called
 */
void
server_instance_run(void)
{
    g_assert(RUNNING);
    g_assert(!THREAD_RUNNING);

    g_main_context_pop_thread_default(CONTEXT);
    LOOP_THREAD = g_thread_new("clippor-test", server_thread, NULL);
    THREAD_RUNNING = TRUE;
}

void
server_instance_pause(void)
{
    g_assert(RUNNING);
    g_assert(THREAD_RUNNING);

    // Make sure to quit loop in the thread it was spawned in
    g_main_context_invoke_full(
        CONTEXT, G_PRIORITY_LOW, server_thread_quit, NULL, NULL
    );
    g_thread_join(LOOP_THREAD);
    g_thread_unref(LOOP_THREAD);
    g_main_context_push_thread_default(CONTEXT);
    THREAD_RUNNING = FALSE;
}

void
server_instance_stop(void)
{
    g_assert(RUNNING);
    g_assert(!THREAD_RUNNING);

    server_free(SERVER_FLAG_NONE);
    g_main_context_pop_thread_default(CONTEXT);
    g_main_context_unref(CONTEXT);
    g_free(CONFIG_CONTENTS);
    RUNNING = FALSE;
}

void
server_instance_dispatch_and_run(void)
{
    server_instance_dispatch();
    server_instance_run();
}

void
server_instance_restart(void)
{
    g_assert(RUNNING);
    g_assert(!THREAD_RUNNING);

    server_free(SERVER_FLAG_NO_UNINIT_DB);
    g_assert_true(server_start(
        CONFIG_CONTENTS, NULL,
        SERVER_FLAG_DB_IN_MEMORY | SERVER_FLAG_MANUAL | SERVER_FLAG_NO_INIT_DB
    ));
}
