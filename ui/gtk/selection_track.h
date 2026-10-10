/* GTK container drawing a selection highlight beneath one child that glides between rows. */
#ifndef TORCHLIGHT_GTK_SELECTION_TRACK_H
#define TORCHLIGHT_GTK_SELECTION_TRACK_H
#include <gtk/gtk.h>
#include <stdbool.h>
/** Create a floating track around borrowed floating child, which it parents and
 * sizes. The highlight is a decorative, empty "selection-highlight" box, hidden
 * from accessibility. Main thread only; GTK handles allocation
 * failure. Caller parents/owns the returned widget. */
GtkWidget *popup_selection_track_new(GtkWidget *child);
/** Move the highlight to target, a descendant of the track's child, or hide it
 * when target is NULL. glide animates from the current place when animations
 * are enabled and the highlight is already shown; otherwise it jumps. The track
 * keeps a reference to target until the next call or dispose. No errors. */
void popup_selection_track_set_target(GtkWidget *track, GtkWidget *target, bool glide);
/** Borrow the current target, or NULL; valid until the next set_target call. */
GtkWidget *popup_selection_track_get_target(GtkWidget *track);
/** Borrow the highlight widget for styling; valid while the track lives. */
GtkWidget *popup_selection_track_get_highlight(GtkWidget *track);
#endif
