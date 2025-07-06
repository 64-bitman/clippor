#include "test_util.h"
#include <glib.h>
#include <stdio.h>
#include <unistd.h>

struct _TesterClient
{
    ClipporClient parent;

    TesterClientSelection regular;
    TesterClientSelection primary;
};

G_DEFINE_TYPE(TesterClient, tester_client, CLIPPOR_TYPE_CLIENT)

static GBytes *tester_client_get_data(
    ClipporClient *self, const char *mime_type, ClipporSelectionType selection,
    GError **error
);
static GPtrArray *tester_client_get_mime_types(
    ClipporClient *self, ClipporSelectionType selection
);
static gboolean tester_client_set_entry(
    ClipporClient *self, ClipporEntry *entry, ClipporSelectionType selection,
    GError **error
);

static void
tester_client_dispose(GObject *object)
{
    G_OBJECT_CLASS(tester_client_parent_class)->dispose(object);
}

static void
tester_client_finalize(GObject *object)
{
    TesterClient *self = TESTER_CLIENT(object);

    g_ptr_array_free(self->regular.mime_types, TRUE);
    g_ptr_array_free(self->primary.mime_types, TRUE);

    g_weak_ref_clear(&self->regular.entry);
    g_weak_ref_clear(&self->primary.entry);

    G_OBJECT_CLASS(tester_client_parent_class)->finalize(object);
}

static void
tester_client_class_init(TesterClientClass *class)
{
    ClipporClientClass *clipporclient_class = CLIPPOR_CLIENT_CLASS(class);
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    clipporclient_class->get_data = tester_client_get_data;
    clipporclient_class->get_mime_types = tester_client_get_mime_types;
    clipporclient_class->set_entry = tester_client_set_entry;

    gobject_class->dispose = tester_client_dispose;
    gobject_class->finalize = tester_client_finalize;
}

static void
tester_client_init(TesterClient *self)
{
    self->regular.mime_types = g_ptr_array_new_with_free_func(g_free);
    self->primary.mime_types = g_ptr_array_new_with_free_func(g_free);

    g_weak_ref_init(&self->regular.entry, NULL);
    g_weak_ref_init(&self->primary.entry, NULL);
}

TesterClient *
tester_client_new(void)
{
    return g_object_new(TESTER_TYPE_CLIENT, NULL);
}

static GBytes *
tester_client_get_data(
    ClipporClient *self, const char *mime_type G_GNUC_UNUSED,
    ClipporSelectionType selection, GError **error G_GNUC_UNUSED
)
{
    g_assert(TESTER_IS_CLIENT(self));

    TesterClient *client = TESTER_CLIENT(self);

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        return client->regular.data;
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        return client->primary.data;
    else
        return NULL;
}

static GPtrArray *
tester_client_get_mime_types(
    ClipporClient *self, ClipporSelectionType selection
)
{
    g_assert(TESTER_IS_CLIENT(self));

    TesterClient *client = TESTER_CLIENT(self);

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        return client->regular.mime_types;
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        return client->primary.mime_types;
    else
        return NULL;
}

static gboolean
tester_client_set_entry(
    ClipporClient *self, ClipporEntry *entry, ClipporSelectionType selection,
    GError **error G_GNUC_UNUSED
)
{
    g_assert(TESTER_IS_CLIENT(self));

    TesterClient *client = TESTER_CLIENT(self);

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        g_weak_ref_set(&client->regular.entry, entry);
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        g_weak_ref_set(&client->primary.entry, entry);
    return TRUE;
}

TesterClientSelection *
tester_client_get_selection(TesterClient *self, ClipporSelectionType selection)
{
    g_assert(TESTER_IS_CLIENT(self));

    if (selection == CLIPPOR_SELECTION_TYPE_REGULAR)
        return &self->regular;
    else if (selection == CLIPPOR_SELECTION_TYPE_PRIMARY)
        return &self->primary;
    else
        return NULL;
}

static char *compositor_argv[] = {"labwc", "-c", "NONE", "-d", NULL};

WaylandCompositor *
wayland_compositor_new(gboolean set_env)
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
    g_assert(self != NULL);
    g_assert(self->display != NULL);

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
wl_copy(gboolean primary, char *format, ...)
{
    char *primary_flag = primary ? "-p" : NULL;

    va_list args;

    va_start(args, format);

    char *text = g_strdup_vprintf(format, args);

    va_end(args);

    char *cmdline[] = {"wl-copy", text, primary_flag, NULL};
    GError *error = NULL;
    int status;

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

char *
wl_paste(gboolean primary, gboolean newline, char *mime_type)
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
