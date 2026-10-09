/* Private GTK presentation API; launcher owns IPC and actions, view owns appearance. */
#ifndef TORCHLIGHT_GTK_VIEW_H
#define TORCHLIGHT_GTK_VIEW_H
#include "torchlight/common.h"
#include "torchlight/popup.h"
#include <gtk/gtk.h>
typedef struct popup_view tl_popup_view;
typedef struct {
    GtkWidget *root, *entry, *list, *scroll, *status, *retry;
} tl_popup_widgets;
/** How a result row appears: first results cascade in, later new rows fade. */
typedef enum { POPUP_ENTER_NONE, POPUP_ENTER_CASCADE, POPUP_ENTER_FADE } tl_popup_entrance;
/** Create an owned view on the GTK main thread. Borrow window until destroy.
 * Installs its child. TL_INVALID/NOMEM; out NULL on error. Widgets stay borrowed. */
tl_status popup_view_create(GtkWidget *window, tl_popup_view **out);
/** Return borrowed widget handles for wiring input and IPC; valid until destroy.
 * view must be a live view on the main thread. No allocation/errors. */
tl_popup_widgets popup_view_widgets(tl_popup_view *view);
/** Set status and accessible announcement from borrowed UTF-8 text. Main thread,
 * live view/text required; no ownership transfer or errors. */
void popup_view_set_status(tl_popup_view *view, const char *text);
/** Update result/empty visibility from model count. Main thread; live view.
 * No ownership transfer or errors. Does not alter the entry's text. */
void popup_view_set_results(tl_popup_view *view, size_t count);
/** Build a floating result widget from a borrowed validated model row. path_keep
 * is popup_model_distinct_prefix for the row; entrance and index (its position)
 * choose the appearance animation. Main thread; caller parents it. GTK handles
 * allocation failure; no row retained. */
GtkWidget *popup_view_result(const tl_popup_row *row, size_t path_keep, tl_popup_entrance entrance,
                             size_t index);
/** Move the selection highlight to borrowed list row, or hide it for NULL. glide
 * slides from the previous row when animations are enabled. Main thread; live
 * view. The view keeps a reference to row until the next call. No errors. */
void popup_view_select(tl_popup_view *view, GtkWidget *row, bool glide);
/** Show the searching spinner in place of the search icon, or restore the icon.
 * Main thread; live view. No ownership transfer or errors. */
void popup_view_set_searching(tl_popup_view *view, bool searching);
/** Flash the selected row while its action is being resolved and launched, or
 * clear the flash after failure. Main thread; live view. No errors. */
void popup_view_set_launching(tl_popup_view *view, bool launching);
/** Fit chrome and the result viewport into a logical monitor budget. Main
 * thread; live view, positive width/height. No ownership transfer or errors. */
void popup_view_configure(tl_popup_view *view, int width, int height_limit);
/** Start/stop presentation with window visibility. Active plays the opening
 * effects; any call cancels typing, opening and pending dismissal timers and
 * keeps the window visible. Main thread; live view. No ownership/errors. */
void popup_view_set_active(tl_popup_view *view, bool active);
/** Deactivate and hide the borrowed window, after a short fade when animations
 * are enabled and the window is mapped. set_active and destroy cancel a pending
 * hide. Main thread; live view. No ownership transfer or errors. */
void popup_view_dismiss(tl_popup_view *view);
/** Cancel timers/signals and free owned view; NULL allowed. Main thread, call
 * after destroying its window. All borrowed widget handles expire. */
void popup_view_destroy(tl_popup_view *view);
#endif
