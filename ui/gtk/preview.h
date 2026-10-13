/* Private GTK widget: Tekst's preview panel, the selected result's details (ADR 0048). */
#ifndef TORCHLIGHT_GTK_PREVIEW_H
#define TORCHLIGHT_GTK_PREVIEW_H
#include "torchlight/popup.h"
#include <gtk/gtk.h>
#include <stddef.h>
/** What the panel describes: the selected list item and, for an application, the open
 * windows the list shows under it. */
typedef struct {
    const tl_popup_row *row;                  /* the result, or a child's application */
    tl_popup_item_kind kind;                  /* the selected item: the result or a child */
    const char *windows[POPUP_WINDOWS_SHOWN]; /* single-line UTF-8 titles, most recent first */
    size_t window_count;                      /* titles given */
    size_t windows_open;                      /* all of the application's open windows */
} tl_popup_preview;
/** Create a floating, empty preview panel. Main thread; the caller parents it. */
GtkWidget *popup_preview_new(void);
/** Describe borrowed preview with borrowed icon; both are copied or referenced. A new
 * result waits until the selection rests (70ms), then its lines rise one after
 * another; another item of the result shown updates at once, without rising. The
 * icon, name, kind, windows and actions show first; a worker thread reads what needs
 * the disk (a file's type, size and modified time, a folder's items, an application's
 * description) and fills it in when it arrives. A NULL preview or row clears the
 * panel. A later show, clear or dispose cancels pending reads. Main thread; panel
 * from popup_preview_new. No errors: details that cannot be read are left out. */
void popup_preview_show(GtkWidget *panel, const tl_popup_preview *preview, GIcon *icon);
/** Empty the panel and cancel any pending rest or read. Main thread. No errors. */
void popup_preview_clear(GtkWidget *panel);
/** Show a preview waiting for the selection to rest at once. Main thread. No errors. */
void popup_preview_flush(GtkWidget *panel);
/** Borrow the text of the first shown label with style_class, or NULL, for tests. Main
 * thread; valid until the panel changes. */
const char *popup_preview_text(GtkWidget *panel, const char *style_class);
#endif
