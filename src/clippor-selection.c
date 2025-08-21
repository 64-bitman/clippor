#include "clippor-selection.h"
#include "clippor-entry.h"
#include <glib-object.h>
#include <glib.h>

G_DEFINE_ENUM_TYPE(
    ClipporSelectionType, clippor_selection_type,
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_NONE, "none"),
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_REGULAR, "regular"),
    G_DEFINE_ENUM_VALUE(CLIPPOR_SELECTION_TYPE_PRIMARY, "primary")
)

typedef struct
{
    ClipporSelectionType type;
    ClipporEntry *entry;
} ClipporSelectionPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(
    ClipporSelection, clippor_selection, G_TYPE_OBJECT
)

typedef enum
{
    PROP_TYPE = 1,
    PROP_ENTRY,
    N_PROPERTIES
} ClipporSelectionProperty;

static GParamSpec *obj_properties[N_PROPERTIES] = {NULL};

typedef enum
{
    SIGNAL_UPDATE, // Emitted when there is a new selection available
    N_SIGNALS,
} ClipporSelectionSignal;

static uint obj_signals[N_SIGNALS] = {0};

static void
clippor_selection_set_property(
    GObject *object, guint property_id, const GValue *value, GParamSpec *pspec
)
{
    ClipporSelection *self = CLIPPOR_SELECTION(object);
    ClipporSelectionPrivate *priv =
        clippor_selection_get_instance_private(self);

    switch (property_id)
    {
    case PROP_TYPE:
        priv->type = g_value_get_enum(value);
        break;
    case PROP_ENTRY:
        if (priv->entry == g_value_get_object(value))
            break;
        if (priv->entry != NULL)
            g_object_unref(priv->entry);
        priv->entry = g_value_dup_object(value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
clippor_selection_get_property(
    GObject *object, guint property_id, GValue *value, GParamSpec *pspec
)
{
    ClipporSelection *self = CLIPPOR_SELECTION(object);
    ClipporSelectionPrivate *priv =
        clippor_selection_get_instance_private(self);

    switch (property_id)
    {
    case PROP_TYPE:
        g_value_set_enum(value, priv->type);
        break;
    case PROP_ENTRY:
        g_value_set_object(value, priv->entry);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void
clippor_selection_dispose(GObject *object)
{
    ClipporSelection *self = CLIPPOR_SELECTION(object);
    ClipporSelectionPrivate *priv =
        clippor_selection_get_instance_private(self);

    g_clear_object(&priv->entry);

    G_OBJECT_CLASS(clippor_selection_parent_class)->dispose(object);
}

static void
clippor_selection_finalize(GObject *object)
{
    G_OBJECT_CLASS(clippor_selection_parent_class)->finalize(object);
}

static void
clippor_selection_class_init(ClipporSelectionClass *class)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(class);

    gobject_class->set_property = clippor_selection_set_property;
    gobject_class->get_property = clippor_selection_get_property;

    gobject_class->dispose = clippor_selection_dispose;
    gobject_class->finalize = clippor_selection_finalize;

    obj_properties[PROP_TYPE] = g_param_spec_enum(
        "type", "Type", "Type of selection", CLIPPOR_TYPE_SELECTION_TYPE,
        CLIPPOR_SELECTION_TYPE_NONE, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    );
    obj_properties[PROP_ENTRY] = g_param_spec_object(
        "entry", "Entry", "Entry that selection is set to", CLIPPOR_TYPE_ENTRY,
        G_PARAM_READWRITE
    );

    g_object_class_install_properties(
        gobject_class, N_PROPERTIES, obj_properties
    );

    obj_signals[SIGNAL_UPDATE] = g_signal_new(
        "update", G_TYPE_FROM_CLASS(class),
        G_SIGNAL_NO_HOOKS | G_SIGNAL_NO_RECURSE, 0, NULL, NULL, NULL,
        G_TYPE_NONE, 0
    );
}

static void
clippor_selection_init(ClipporSelection *self G_GNUC_UNUSED)
{
}

/*
 * Get mime types of current selection, creates new reference to array.
 */
GPtrArray *
clippor_selection_get_mime_types(ClipporSelection *self)
{
    g_assert(CLIPPOR_IS_SELECTION(self));

    ClipporSelectionClass *class = CLIPPOR_SELECTION_GET_CLASS(self);
    return class->get_mime_types(self);
}

/*
 * Return a input stream where data for mime type will be written to. This
 * allows for reading data to be asynchronous.
 */
GInputStream *
clippor_selection_get_data(
    ClipporSelection *self, const char *mime_type, GError **error
)
{
    g_assert(CLIPPOR_IS_SELECTION(self));
    g_assert(mime_type != NULL);

    ClipporSelectionClass *class = CLIPPOR_SELECTION_GET_CLASS(self);
    return class->get_data(self, mime_type, error);
}

/*
 * Set the selection for the selection object.
 *
 * If "is_source" is TRUE, then the selection update came from the selection
 * object. The selection object should update its internal state but not
 * explicity set the selection.
 *
 * If "entry" is NULL, the selection is cleared.
 *
 * When data needs to be sent, it should be done asynchronously
 */
gboolean
clippor_selection_update(
    ClipporSelection *self, ClipporEntry *entry, gboolean is_source,
    GError **error
)
{
    g_assert(CLIPPOR_IS_SELECTION(self));
    g_assert(error == NULL || *error == NULL);

    ClipporSelectionClass *class = CLIPPOR_SELECTION_GET_CLASS(self);
    return class->update(self, entry, is_source, error);
}

/*
 * Return TRUE if selection is owned by selection object
 */
gboolean
clippor_selection_is_owned(ClipporSelection *self)
{
    g_assert(CLIPPOR_IS_SELECTION(self));

    ClipporSelectionClass *class = CLIPPOR_SELECTION_GET_CLASS(self);
    return class->is_owned(self);
}

/*
 * Return TRUE if selection object is inert
 */
gboolean
clippor_selection_is_inert(ClipporSelection *self)
{
    g_assert(CLIPPOR_IS_SELECTION(self));

    ClipporSelectionClass *class = CLIPPOR_SELECTION_GET_CLASS(self);
    return class->is_inert(self);
}

ClipporEntry *
clippor_selection_get_entry(ClipporSelection *self)
{
    g_assert(CLIPPOR_IS_SELECTION(self));

    ClipporSelectionPrivate *priv =
        clippor_selection_get_instance_private(self);

    return priv->entry;
}
