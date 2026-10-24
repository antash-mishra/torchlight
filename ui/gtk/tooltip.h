/* Tooltips the popup sets without GTK asking the X server where the pointer is. */
#ifndef TORCHLIGHT_GTK_TOOLTIP_H
#define TORCHLIGHT_GTK_TOOLTIP_H
#include <gtk/gtk.h>
/** Give widget a plain-text tooltip, or none for NULL or "". Unlike
 * gtk_widget_set_tooltip_text, it does not query the pointer at once: GTK 4.14 does that
 * for every visible widget, under a grab of the whole X server. The text, copied, is
 * handed over when GTK asks for the tooltip, and is also the widget's accessible
 * description. Main thread; widget borrowed, text may be freed after the call. No errors. */
void popup_tooltip_set(GtkWidget *widget, const char *text);
/** Return the text popup_tooltip_set gave widget, or NULL; owned by widget until the next
 * call. Main thread. No errors. */
const char *popup_tooltip_text(GtkWidget *widget);
#endif
