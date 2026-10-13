/* Teletext page header: the page number, the service name, the result count and a clock
 * that ticks with slash seconds while the popup is shown (ADR 0048). */
#include "header.h"
#include <stdio.h>
#define HEADER_SPACING 18
#define CLOCK_BYTES 64
#define COUNT_BYTES 32
#define MS_PER_SECOND 1000
#define US_PER_MS 1000
typedef struct {
    GtkWidget parent;
    GtkWidget *page, *service, *count, *clock;
    guint tick;
} TlPopupHeader;
typedef struct {
    GtkWidgetClass parent;
} TlPopupHeaderClass;
G_DEFINE_TYPE(TlPopupHeader, tl_popup_header, GTK_TYPE_WIDGET)
static GtkWidget *part(TlPopupHeader *self, const char *text, const char *class) {
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_add_css_class(label, class);
    gtk_widget_set_parent(label, GTK_WIDGET(self));
    return label;
}
/* Weekday, day and month, then teletext's time with slash seconds: Tue 13 Oct  21:52/07. */
static void show_time(TlPopupHeader *self) {
    GDateTime *now = g_date_time_new_now_local();
    if (now == NULL)
        return;
    char *weekday = g_date_time_format(now, "%a"), *month = g_date_time_format(now, "%b");
    char text[CLOCK_BYTES];
    int written = snprintf(text, sizeof(text), "%s %d %s  %02d:%02d/%02d",
                           weekday != NULL ? weekday : "", g_date_time_get_day_of_month(now),
                           month != NULL ? month : "", g_date_time_get_hour(now),
                           g_date_time_get_minute(now), g_date_time_get_second(now));
    if (written > 0 && (size_t)written < sizeof(text))
        gtk_label_set_text(GTK_LABEL(self->clock), text);
    g_free(weekday);
    g_free(month);
    g_date_time_unref(now);
}
static gboolean tick(gpointer context);
/* Wake just after the next whole second, so the seconds change on time. */
static void schedule(TlPopupHeader *self) {
    gint64 into_second = (g_get_real_time() / US_PER_MS) % MS_PER_SECOND;
    self->tick = g_timeout_add((guint)(MS_PER_SECOND - into_second) + 1, tick, self);
}
static gboolean tick(gpointer context) {
    TlPopupHeader *self = context;
    show_time(self);
    schedule(self);
    return G_SOURCE_REMOVE;
}
static void stop(TlPopupHeader *self) {
    if (self->tick != 0)
        g_source_remove(self->tick);
    self->tick = 0;
}
static void dispose(GObject *object) {
    TlPopupHeader *self = (TlPopupHeader *)object;
    stop(self);
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(self)); child != NULL;
         child = gtk_widget_get_first_child(GTK_WIDGET(self)))
        gtk_widget_unparent(child);
    G_OBJECT_CLASS(tl_popup_header_parent_class)->dispose(object);
}
static void tl_popup_header_class_init(TlPopupHeaderClass *class) {
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(class);
    gtk_widget_class_set_layout_manager_type(widget, GTK_TYPE_BOX_LAYOUT);
    G_OBJECT_CLASS(class)->dispose = dispose;
}
static void tl_popup_header_init(TlPopupHeader *self) {
    gtk_widget_add_css_class(GTK_WIDGET(self), "page-header");
    gtk_widget_set_name(GTK_WIDGET(self), "popup-header");
    gtk_box_layout_set_spacing(GTK_BOX_LAYOUT(gtk_widget_get_layout_manager(GTK_WIDGET(self))),
                               HEADER_SPACING);
    self->page = part(self, "P100", "page-number");
    self->service = part(self, "Torchlight", "service");
    self->count = part(self, "", "result-count");
    gtk_widget_set_hexpand(self->count, true);
    gtk_label_set_xalign(GTK_LABEL(self->count), 1);
    self->clock = part(self, "", "clock");
    gtk_accessible_update_state(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_STATE_HIDDEN, true, -1);
}
GtkWidget *popup_header_new(void) {
    return g_object_new(tl_popup_header_get_type(), NULL);
}
void popup_header_set_count(GtkWidget *header, size_t count) {
    TlPopupHeader *self = (TlPopupHeader *)header;
    char text[COUNT_BYTES] = "";
    if (count != 0) {
        int written = snprintf(text, sizeof(text), "%zu result%s", count, count == 1 ? "" : "s");
        if (written < 0 || (size_t)written >= sizeof(text))
            text[0] = 0;
    }
    gtk_label_set_text(GTK_LABEL(self->count), text);
}
void popup_header_set_running(GtkWidget *header, bool running) {
    TlPopupHeader *self = (TlPopupHeader *)header;
    stop(self);
    if (!running)
        return;
    show_time(self);
    schedule(self);
}
const char *popup_header_clock(GtkWidget *header) {
    return gtk_label_get_text(GTK_LABEL(((TlPopupHeader *)header)->clock));
}
