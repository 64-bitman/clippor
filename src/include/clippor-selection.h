#pragma once

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

    const GPtrArray *(*get_mime_types)(ClipporSelection *self);
    void (*start_get_data)(
        ClipporSelection *self, const char *mime_type, GByteArray *buf
    );
    void (*cancel_get_data)(ClipporSelection *self);
    gboolean (*set_data)(
        ClipporSelection *self, GHashTable *mime_types, GError **error
    );
    gboolean (*is_owned)(ClipporSelection *self);
};

const GPtrArray *clippor_selection_get_mime_types(ClipporSelection *self);
void clippor_selection_start_get_data(
    ClipporSelection *self, const char *mime_type, GByteArray *buf
);
gboolean clippor_selection_set_data(
    ClipporSelection *self, GHashTable *mime_types, GError **error
);
gboolean clippor_selection_is_owned(ClipporSelection *self);
