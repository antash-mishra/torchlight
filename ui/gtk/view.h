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
/** Build a floating result widget from a borrowed validated model row. Main
 * thread; caller parents it. GTK handles allocation failure; no row retained. */
GtkWidget *popup_view_result(const tl_popup_row *row);
/** Fit chrome and the result viewport into a logical monitor budget. Main
 * thread; live view, positive width/height. No ownership transfer or errors. */
void popup_view_configure(tl_popup_view *view, int width, int height_limit);
/** Start/stop presentation timers with window visibility. Main thread; live view.
 * Inactive cancels all view timers. No ownership transfer or errors. */
void popup_view_set_active(tl_popup_view *view, bool active);
/** Cancel timers/signals and free owned view; NULL allowed. Main thread, call
 * after destroying its window. All borrowed widget handles expire. */
void popup_view_destroy(tl_popup_view *view);
#endif
