#include "dummy-selection.h"
#include "clippor-selection.h"
#include <glib-object.h>
#include <glib-unix.h>
#include <glib.h>
#include <stdarg.h>
#include <stdint.h>

// Emulates the behaviour of an actual selection, used for testing

struct _DummySelection
{
    ClipporSelection parent_instance;

    GHashTable *mime_types;

    // Emulated after Wayland model
    gboolean has_source;
    gboolean has_offer;

    // Holds string of last paste. Makes it so that the caller doesn't have to
    // free the string returned by dummy_selection_paste.
    char *paste;

    GSource *source;

    int pipe_fds[2];

    gboolean active; // If pipe is still valid
};

G_DEFINE_TYPE(DummySelection, dummy_selection, CLIPPOR_TYPE_SELECTION)

static void
dummy_selection_dispose(GObject *object)
{
    DummySelection *self = DUMMY_SELECTION(object);

    g_clear_pointer(&self->mime_types, g_hash_table_unref);

    G_OBJECT_CLASS(dummy_selection_parent_class)->dispose(object);
}

static void
dummy_selection_finalize(GObject *object)
{
    DummySelection *self = DUMMY_SELECTION(object);

    dummy_selection_uninstall_source(self);

    close(self->pipe_fds[0]);
    close(self->pipe_fds[1]);

    g_free(self->paste);

    G_OBJECT_CLASS(dummy_selection_parent_class)->finalize(object);
}

static GPtrArray *
clippor_selection_handler_get_mime_types(ClipporSelection *self);
static GInputStream *clippor_selection_handler_get_data(
    ClipporSelection *self, const char *mime_type, GError **error
);
static gboolean clippor_selection_handler_update(
    ClipporSelection *self, ClipporEntry *entry, gboolean is_source,
    GError **error
);
static gboolean clippor_selection_handler_is_owned(ClipporSelection *self);
static gboolean clippor_selection_handler_is_inert(ClipporSelection *self);

static void
dummy_selection_class_init(DummySelectionClass *class)
{

    GObjectClass *gobject_class = G_OBJECT_CLASS(class);
    ClipporSelectionClass *sel_class = CLIPPOR_SELECTION_CLASS(class);

    gobject_class->dispose = dummy_selection_dispose;
    gobject_class->finalize = dummy_selection_finalize;

    sel_class->get_mime_types = clippor_selection_handler_get_mime_types;
    sel_class->get_data = clippor_selection_handler_get_data;
    sel_class->update = clippor_selection_handler_update;
    sel_class->is_owned = clippor_selection_handler_is_owned;
    sel_class->is_inert = clippor_selection_handler_is_inert;
}

static void
dummy_selection_init(DummySelection *self)
{
    self->mime_types = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref
    );

    g_assert_true(g_unix_open_pipe(self->pipe_fds, O_CLOEXEC, NULL));
    self->active = TRUE;
}

DummySelection *
dummy_selection_new(ClipporSelectionType type)
{
    g_assert(type != CLIPPOR_SELECTION_TYPE_NONE);

    DummySelection *dsel =
        g_object_new(DUMMY_TYPE_SELECTION, "type", type, NULL);

    return dsel;
}

static void
dummy_selection_own(DummySelection *self)
{
    g_assert(DUMMY_IS_SELECTION(self));

    ClipporEntry *entry = clippor_selection_get_entry(CLIPPOR_SELECTION(self));

    if (entry == NULL)
    {
        // Make selection cleared
        g_hash_table_remove_all(self->mime_types);
        self->has_offer = FALSE;
        self->has_source = FALSE;
        return;
    }

    GHashTableIter iter;
    const char *mime_type;
    GBytes *bytes;

    g_hash_table_iter_init(&iter, clippor_entry_get_mime_types(entry));

    while (g_hash_table_iter_next(&iter, (void **)&mime_type, (void **)&bytes))
    {
        g_assert_nonnull(mime_type);
        g_assert_nonnull(bytes);

        g_hash_table_insert(
            self->mime_types, g_strdup(mime_type), g_bytes_ref(bytes)
        );
    }
    self->has_offer = FALSE;
    self->has_source = TRUE;
}

static int
dummy_selection_pipe_callback(int fd, GIOCondition condition, void *user_data)
{
    DummySelection *self = user_data;

    char buf[1];

    read(fd, buf, 1);
    g_assert_cmpint(buf[0], ==, 1);

    if (condition & (G_IO_HUP | G_IO_ERR))
    {
        close(fd);
        self->active = FALSE;
        return G_SOURCE_REMOVE;
    }

    self->has_source = FALSE;

    if (!self->has_offer)
    {
        ClipporEntry *entry =
            clippor_selection_get_entry(CLIPPOR_SELECTION(self));

        if (entry != NULL)
            dummy_selection_own(self);
    }
    else
        g_signal_emit_by_name(CLIPPOR_SELECTION(self), "update");

    return G_SOURCE_CONTINUE;
}

void
dummy_selection_install_source(DummySelection *self, GMainContext *context)
{
    g_assert(DUMMY_IS_SELECTION(self));

    self->source =
        g_unix_fd_source_new(self->pipe_fds[0], G_IO_IN | G_IO_HUP | G_IO_ERR);

    g_source_set_static_name(self->source, "Dummy selection");

    g_source_set_callback(
        self->source, G_SOURCE_FUNC(dummy_selection_pipe_callback), self, NULL
    );

    g_source_set_priority(self->source, G_MININT);
    g_source_attach(self->source, context);
}

void
dummy_selection_uninstall_source(DummySelection *self)
{
    g_assert(DUMMY_IS_SELECTION(self));

    if (self->source == NULL)
        return;

    g_source_destroy(self->source);
    g_clear_pointer(&self->source, g_source_unref);
}

/*
 * Copy text to the selection. If "first_mime_type" is NULL, then the selection
 * is cleared and "contents" is ignored.
 */
void
dummy_selection_copy(
    DummySelection *self, const char *contents, const char *first_mime_type, ...
)
{
    g_assert(DUMMY_IS_SELECTION(self));
    g_assert(first_mime_type == NULL || contents != NULL);

    if (first_mime_type == NULL)
    {
        self->has_offer = FALSE;
        goto exit;
    }

    GBytes *data = g_bytes_new(contents, strlen(contents));
    const char *mime_type = first_mime_type;
    va_list ap;

    va_start(ap, first_mime_type);

    while (mime_type != NULL)
    {
        g_hash_table_insert(
            self->mime_types, g_strdup(mime_type), g_bytes_ref(data)
        );
        mime_type = va_arg(ap, char *);
    }

    va_end(ap);
    g_bytes_unref(data);

    self->has_offer = TRUE;
exit:;
    char buf[1] = {1};
    write(self->pipe_fds[1], buf, 1);
}

/*
 * Paste test from the selection, else NULL if selection is cleared or no mime
 * type.
 */
const char *
dummy_selection_paste(DummySelection *self, const char *mime_type)
{
    g_assert(DUMMY_IS_SELECTION(self));
    g_assert(mime_type != NULL);

    GBytes *bytes = g_hash_table_lookup(self->mime_types, mime_type);

    if (bytes == NULL)
    {
        g_warning("No mime type %s in dummy selection", mime_type);
        return NULL;
    }

    size_t sz;
    const uint8_t *data = g_bytes_get_data(bytes, &sz);

    g_free(self->paste);
    self->paste = g_strdup_printf("%.*s", (int)sz, data);

    return self->paste;
}

static GPtrArray *
clippor_selection_handler_get_mime_types(ClipporSelection *self)
{
    g_assert(DUMMY_IS_SELECTION(self));

    DummySelection *dsel = DUMMY_SELECTION(self);

    return g_hash_table_get_keys_as_ptr_array(dsel->mime_types);
}

static GInputStream *
clippor_selection_handler_get_data(
    ClipporSelection *self, const char *mime_type, GError **error
)
{
    g_assert(DUMMY_IS_SELECTION(self));
    g_assert(mime_type != NULL);
    g_assert(error == NULL || *error == NULL);

    DummySelection *dsel = DUMMY_SELECTION(self);
    GBytes *data = g_hash_table_lookup(dsel->mime_types, mime_type);

    if (data == NULL)
        return NULL;
    return g_memory_input_stream_new_from_bytes(data);
}

static gboolean
clippor_selection_handler_update(
    ClipporSelection *self, ClipporEntry *entry, gboolean is_source,
    GError **error
)
{
    g_assert(DUMMY_IS_SELECTION(self));
    g_assert(entry == NULL || CLIPPOR_IS_ENTRY(entry));
    g_assert(error == NULL || *error == NULL);

    DummySelection *dsel = DUMMY_SELECTION(self);

    g_object_set(dsel, "entry", entry, NULL);

    if (!is_source)
        dummy_selection_own(dsel);

    return TRUE;
}

static gboolean
clippor_selection_handler_is_owned(ClipporSelection *self)
{
    g_assert(DUMMY_IS_SELECTION(self));

    DummySelection *dsel = DUMMY_SELECTION(self);

    return dsel->has_source;
}

static gboolean
clippor_selection_handler_is_inert(ClipporSelection *self)
{
    g_assert(DUMMY_IS_SELECTION(self));

    DummySelection *dsel = DUMMY_SELECTION(self);

    return !dsel->active;
}
