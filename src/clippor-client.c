#include "clippor-client.h"
#include "clippor-entry.h"
#include <glib-object.h>
#include <glib.h>

G_DEFINE_ABSTRACT_TYPE(ClipporClient, clippor_client, G_TYPE_OBJECT)

typedef enum
{
    SIGNAL_SELECTION,
    N_SIGNALS
} ClipporClientSignal;

static uint obj_signals[N_SIGNALS] = {0};

static void
clippor_client_class_init(ClipporClientClass *class)
{
    class->get_mime_types = NULL;
    class->get_data = NULL;
    class->set_entry = NULL;

    // Used to notify any clipboards that we have a new selection now
    obj_signals[SIGNAL_SELECTION] = g_signal_new(
        "selection", G_TYPE_FROM_CLASS(class),
        G_SIGNAL_RUN_LAST | G_SIGNAL_NO_HOOKS | G_SIGNAL_NO_RECURSE |
            G_SIGNAL_DETAILED,
        0, NULL, NULL, NULL, G_TYPE_NONE, 1, CLIPPOR_TYPE_SELECTION_TYPE
    );
}
static void
clippor_client_init(ClipporClient *self G_GNUC_UNUSED)
{
}

/*
 * Return mime types for the current selection or NULL if there is no selection.
 * Should return a new reference to ptr array.
 */
GPtrArray *
clippor_client_get_mime_types(
    ClipporClient *self, ClipporSelectionType selection
)
{
    g_assert(CLIPPOR_IS_CLIENT(self));
    g_assert(selection != CLIPPOR_SELECTION_TYPE_NONE);

    ClipporClientClass *class = CLIPPOR_CLIENT_GET_CLASS(self);

    if (class->get_mime_types == NULL)
        return NULL;

    GPtrArray *mime_types = class->get_mime_types(self, selection);

    return mime_types == NULL ? NULL : g_ptr_array_ref(mime_types);
}

/*
 * Returns data for given mime type for the current selection. Returns NULL on
 * error or if there is no selection.
 */
GBytes *
clippor_client_get_data(
    ClipporClient *self, const char *mime_type, ClipporSelectionType selection,
    GError **error
)
{
    g_assert(CLIPPOR_IS_CLIENT(self));
    g_assert(mime_type != NULL);
    g_assert(selection != CLIPPOR_SELECTION_TYPE_NONE);
    g_assert(error == NULL || *error == NULL);

    ClipporClientClass *class = CLIPPOR_CLIENT_GET_CLASS(self);

    return class->get_data == NULL
               ? NULL
               : class->get_data(self, mime_type, selection, error);
}

/*
 * Sets the current entry of the client, essentially setting the selection. If
 * "update" is TRUE, then only set the selection if the current entry has been
 * removed/does not exist, else clear the selection.
 */
gboolean
clippor_client_set_entry(
    ClipporClient *self, ClipporEntry *entry, ClipporSelectionType selection,
    gboolean update, GError **error
)
{
    g_assert(CLIPPOR_IS_CLIENT(self));
    g_assert(selection != CLIPPOR_SELECTION_TYPE_NONE);
    g_assert(error == NULL || *error == NULL);

    ClipporClientClass *class = CLIPPOR_CLIENT_GET_CLASS(self);

    //  Don't want to immediately steal the selection when there is a new one
    //  (Only if selection is the same).
    if (entry != NULL && clippor_entry_get_selection(entry) == selection)
        if (clippor_entry_is_from(entry) == self)
            return TRUE;

    if (entry != NULL)
    {
        GHashTableIter iter;

        g_hash_table_iter_init(&iter, clippor_entry_get_mime_types(entry));

        // No point in setting selection if there are no mime types in entry
        if (!g_hash_table_iter_next(&iter, NULL, NULL))
            return TRUE;
    }

    return class->set_entry == NULL
               ? FALSE
               : class->set_entry(self, entry, selection, update, error);
}
