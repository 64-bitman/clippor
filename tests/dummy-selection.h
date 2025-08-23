#include "clippor-selection.h"
#include <glib-object.h>
#include <glib.h>

G_DECLARE_FINAL_TYPE(
    DummySelection, dummy_selection, DUMMY, SELECTION, ClipporSelection
)
#define DUMMY_TYPE_SELECTION (dummy_selection_get_type())

DummySelection *dummy_selection_new(ClipporSelectionType type);

void
dummy_selection_install_source(DummySelection *self, GMainContext *context);
void dummy_selection_uninstall_source(DummySelection *self);

void dummy_selection_copy(
    DummySelection *self, const char *contents, const char *first_mime_type, ...
);
const char *dummy_selection_paste(DummySelection *self, const char *mime_type);
