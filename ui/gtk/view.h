/* Private GTK presentation API; launcher owns IPC and actions, view owns appearance. */
#ifndef TORCHLIGHT_GTK_VIEW_H
#define TORCHLIGHT_GTK_VIEW_H
#include "index_strip.h"
#include "preview.h"
#include "torchlight/common.h"
#include "torchlight/popup.h"
#include <gtk/gtk.h>
/* The visible surface's width with the results alone, and with the preview panel beside
 * them; the launcher picks the wide one when the work area has room for it. */
#define POPUP_VIEW_LIST_WIDTH 680
#define POPUP_VIEW_PREVIEW_WIDTH 880
typedef struct popup_view tl_popup_view;
typedef struct {
    GtkWidget *root, *entry, *list, *scroll, *status, *retry, *preview;
} tl_popup_widgets;
/** How a result row appears: first results cascade in, later new rows fade. */
typedef enum { POPUP_ENTER_NONE, POPUP_ENTER_CASCADE, POPUP_ENTER_FADE } tl_popup_entrance;
/** The transparent margin around the visible surface, in logical pixels, where its
 * shadow is drawn. All zero without a compositor, with GTK's software (Cairo)
 * renderer or in high contrast. */
typedef struct {
    int left, right, top, bottom;
} tl_popup_margins;
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
/** Build a floating section header for group: its teletext color block, name
 * ("Apps", "Settings", "Folders" or "Files") and count of results. Hidden from
 * assistive technology, which reads each row. Main thread; the caller sets it as
 * a list row's header. No errors. */
GtkWidget *popup_view_group_header(tl_popup_group group, size_t count);
/** Build a compact floating list row beneath borrowed application parent for a
 * child item kind: an open window (its title), more windows or new window. The
 * row's content carries the kind as a style class ("window", "more-windows" or
 * "new-window") besides "kind-window". text is borrowed single-line UTF-8 and
 * copied; entrance and index choose the appearance animation. Returns a
 * GtkListBoxRow for the caller to append. Main thread; no row retained or errors. */
GtkWidget *popup_view_child(const tl_popup_row *parent, tl_popup_item_kind kind, const char *text,
                            tl_popup_entrance entrance, size_t index);
/** Move the selection highlight to borrowed list row, or hide it for NULL. glide
 * slides from the previous row when animations are enabled; the highlight takes
 * the row's kind class (kind-app, kind-setting, kind-folder, kind-file or
 * kind-window) for its colour. Main thread; live view. The view keeps a reference
 * to row until the next call. No errors. */
void popup_view_select(tl_popup_view *view, GtkWidget *row, bool glide);
/** Show the searching spinner in place of the search icon, or restore the icon.
 * Main thread; live view. No ownership transfer or errors. */
void popup_view_set_searching(tl_popup_view *view, bool searching);
/** Flash the selected row while its action is being resolved and launched, or
 * clear the flash after failure. Main thread; live view. No errors. */
void popup_view_set_launching(tl_popup_view *view, bool launching);
/** Fit chrome and the result viewport into a logical monitor budget: width is the
 * visible surface's, height_limit the whole window's. A width of at least
 * POPUP_VIEW_PREVIEW_WIDTH shows the preview panel beside the results. Also decides, from
 * the display, the window's renderer and the theme, whether the surface has a shadow
 * margin (popup_view_margins); call it after realizing the window. Main thread; live
 * view, positive width/height. No ownership transfer or errors. */
void popup_view_configure(tl_popup_view *view, int width, int height_limit);
/** Return the shadow margin chosen by the last popup_view_configure; the window
 * must be this much larger than the surface on each side. Clicks in it reach the
 * windows beneath. Main thread; live view. No ownership transfer or errors. */
tl_popup_margins popup_view_margins(tl_popup_view *view);
/** Describe the borrowed selected item in the preview panel (see popup_preview_show),
 * with the same icon as its row; a NULL preview or row clears it. A new result shows
 * once the selection rests, or at once with at_once (new results, so the window grows
 * once). Only a wide popup shows the panel, so a narrow one clears it and reads nothing.
 * Main thread; live view. No errors. */
void popup_view_set_preview(tl_popup_view *view, const tl_popup_preview *preview, bool at_once);
/** Show the borrowed index state in the idle strip under an empty query. Main
 * thread; live view. No ownership transfer or errors. */
void popup_view_set_index(tl_popup_view *view, const tl_popup_index *index);
/** Confirm a copied path: the Copy path link says "Copied" for a moment and screen
 * readers hear it. Main thread; live view. No errors. */
void popup_view_set_copied(tl_popup_view *view);
/** Start/stop presentation with window visibility. Active plays the opening
 * effects, starts the header clock and shows the next placeholder tip; any call
 * cancels typing, opening and pending dismissal timers and keeps the window
 * visible; inactive stops the clock. Main thread; live view. No ownership/errors. */
void popup_view_set_active(tl_popup_view *view, bool active);
/** Deactivate and hide the borrowed window, after a short fade when animations
 * are enabled and the window is mapped. set_active and destroy cancel a pending
 * hide. Main thread; live view. No ownership transfer or errors. */
void popup_view_dismiss(tl_popup_view *view);
/** Whether each hide also drops the window's surface (unrealizes it), so that the next
 * show creates a new one. Compositors whose gtk_shell1 is older than version 4, such as
 * Cinnamon's, need it: GTK 4 cannot release a hidden window's gtk_surface1 there, and asking
 * for one again on the same surface is a protocol error that disconnects the popup. Off by
 * default. Main thread; live view. No errors. */
void popup_view_set_fresh_surfaces(tl_popup_view *view, bool fresh);
/** Cancel timers/signals and free owned view; NULL allowed. Main thread, call
 * after destroying its window. All borrowed widget handles expire. */
void popup_view_destroy(tl_popup_view *view);
#endif
