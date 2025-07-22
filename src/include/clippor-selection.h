#pragma once

#include "clippor-entry.h"
#include <gio/gio.h>
#include <glib-object.h>
#include <glib.h>

typedef enum
{
    CLIPPOR_SELECTION_TYPE_NONE,
    CLIPPOR_SELECTION_TYPE_REGULAR,
    CLIPPOR_SELECTION_TYPE_PRIMARY
} ClipporSelectionType;

#define CLIPPOR_TYPE_SELECTION_TYPE (clippor_selection_type_get_type())
GType clippor_selection_type_get_type(void);

G_DECLARE_DERIVABLE_TYPE(
    ClipporSelection, clippor_selection, CLIPPOR, SELECTION, GObject
)
#define CLIPPOR_TYPE_SELECTION (clippor_selection_get_type())

struct _ClipporSelectionClass
{
    GObjectClass parent_class;

    GPtrArray *(*get_mime_types)(ClipporSelection *self);
    GInputStream *(*get_data)(
        ClipporSelection *self, const char *mime_type, GError **error
    );
    gboolean (*update)(
        ClipporSelection *self, ClipporEntry *entry, gboolean is_source,
        GError **error
    );
    gboolean (*is_owned)(ClipporSelection *self);
    gboolean (*is_inert)(ClipporSelection *self);
};

GPtrArray *clippor_selection_get_mime_types(ClipporSelection *self);
GInputStream *clippor_selection_get_data(
    ClipporSelection *self, const char *mime_type, GError **error
);
gboolean clippor_selection_update(
    ClipporSelection *self, ClipporEntry *entry, gboolean is_source,
    GError **error
);
gboolean clippor_selection_is_owned(ClipporSelection *self);
gboolean clippor_selection_is_inert(ClipporSelection *self);

ClipporEntry *clippor_selection_get_entry(ClipporSelection *self);
