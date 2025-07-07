#pragma once

#include "clippor-clipboard.h"
#include <glib-object.h>
#include <glib.h>

#define CLIPPOR_TYPE_CLIENT (clippor_client_get_type())

G_DECLARE_DERIVABLE_TYPE(
    ClipporClient, clippor_client, CLIPPOR, CLIENT, GObject
)

struct _ClipporClientClass
{
    GObjectClass parent_class;

    // Properties
    char *label;

    // Non-properties, these are set via a class method
    struct
    {
        GWeakRef entry;
    } regular;
    struct
    {
        GWeakRef entry;
    } primary;

    // Class methods
    GPtrArray *(*get_mime_types)(
        ClipporClient *self, ClipporSelectionType selection
    );
    GBytes *(*get_data)(
        ClipporClient *self, const char *mime_type,
        ClipporSelectionType selection, GError **error
    );
    gboolean (*set_entry)(
        ClipporClient *self, ClipporEntry *entry,
        ClipporSelectionType selection, gboolean update, GError **error
    );
};

GPtrArray *clippor_client_get_mime_types(
    ClipporClient *self, ClipporSelectionType selection
);
GBytes *clippor_client_get_data(
    ClipporClient *self, const char *mime_type, ClipporSelectionType selection,
    GError **error
);
gboolean clippor_client_set_entry(
    ClipporClient *self, ClipporEntry *entry, ClipporSelectionType selection,
    gboolean update, GError **error
);
