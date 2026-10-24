/* Tooltips set without a pointer query. gtk_widget_set_tooltip_text asks where the pointer
 * is each time a visible widget gets tooltip text, in case the tooltip should change under
 * it, and on X11 GTK 4.14 grabs the whole X server to find the window there. Under GNOME's
 * X11 compositor a server grab waited for the next screen refresh or two (16 to 35 ms,
 * against 0.1 ms for a query without one), so the two tooltips of every result row stalled
 * the popup for 100 to 300 ms per keystroke. The text waits here until GTK asks. */
#include "tooltip.h"
#include <stdbool.h>
#define TOOLTIP_TEXT "torchlight-tooltip"
#define TOOLTIP_CONNECTED "torchlight-tooltip-connected"
static gboolean query_tooltip(GtkWidget *widget, int x, int y, gboolean keyboard,
                              GtkTooltip *tooltip, gpointer context) {
    (void)x;
    (void)y;
    (void)keyboard;
    (void)context;
    const char *text = popup_tooltip_text(widget);
    if (text == NULL)
        return false;
    gtk_tooltip_set_text(tooltip, text);
    return true;
}
void popup_tooltip_set(GtkWidget *widget, const char *text) {
    bool shown = text != NULL && *text != '\0';
    g_object_set_data_full(G_OBJECT(widget), TOOLTIP_TEXT, shown ? g_strdup(text) : NULL, g_free);
    if (shown && g_object_get_data(G_OBJECT(widget), TOOLTIP_CONNECTED) == NULL) {
        g_signal_connect(widget, "query-tooltip", G_CALLBACK(query_tooltip), NULL);
        g_object_set_data(G_OBJECT(widget), TOOLTIP_CONNECTED, GINT_TO_POINTER(1));
    }
    /* Setting has-tooltip itself never queries the pointer. */
    gtk_widget_set_has_tooltip(widget, shown);
    if (shown)
        gtk_accessible_update_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                       text, -1);
    else
        gtk_accessible_reset_property(GTK_ACCESSIBLE(widget), GTK_ACCESSIBLE_PROPERTY_DESCRIPTION);
}
const char *popup_tooltip_text(GtkWidget *widget) {
    return g_object_get_data(G_OBJECT(widget), TOOLTIP_TEXT);
}
