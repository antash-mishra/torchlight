/* Teletext page header: the page number, the service name, how long the search took and a
 * clock that ticks with slash seconds while the popup is shown (ADR 0048, ADR 0055). */
#include "header.h"
#include "torchlight/popup.h"
#include <inttypes.h>
#include <stdio.h>
#define HEADER_SPACING 18
#define CLOCK_BYTES 64
/* A two-digit day and the last second of a day: the clock's longest text in a month. */
#define WIDEST_DAY 28
#define LAST_HOUR 23
#define LAST_MINUTE 59
#define LAST_SECOND 59
#define TIME_BYTES 16
/* The widest search time, "<0.1ms": the field keeps this width so the header never shifts. */
#define TIME_CHARS 6
#define MS_PER_SECOND 1000
#define US_PER_MS 1000
#define US_PER_TENTH_MS 100
#define US_PER_TENTH_SECOND 100000
/* Below ten milliseconds the time shows tenths; below a second, whole milliseconds. */
#define TENTHS_SHOWN 100
#define WHOLE_MS_BELOW 999500
typedef struct {
    GtkWidget parent;
    GtkWidget *page, *service, *time, *clock;
    guint tick;
    /* The clock's own text while it shows its widest (set_widest). */
    char *saved_clock;
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
/* Weekday, day and month, then teletext's time with slash seconds: Tue 13 Oct  21:52/07.
 * Empty when it does not fit. */
static void clock_text(GDateTime *when, char *text, size_t size) {
    char *weekday = g_date_time_format(when, "%a"), *month = g_date_time_format(when, "%b");
    int written = snprintf(text, size, "%s %d %s  %02d:%02d/%02d", weekday != NULL ? weekday : "",
                           g_date_time_get_day_of_month(when), month != NULL ? month : "",
                           g_date_time_get_hour(when), g_date_time_get_minute(when),
                           g_date_time_get_second(when));
    if (written < 0 || (size_t)written >= size)
        text[0] = 0;
    g_free(weekday);
    g_free(month);
}
static void show_time(TlPopupHeader *self) {
    GDateTime *now = g_date_time_new_now_local();
    if (now == NULL)
        return;
    char text[CLOCK_BYTES];
    clock_text(now, text, sizeof(text));
    if (text[0] != 0)
        gtk_label_set_text(GTK_LABEL(self->clock), text);
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
    g_clear_pointer(&self->saved_clock, g_free);
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
    /* The time takes the free width, its text kept beside the clock. */
    self->time = part(self, "", "search-time");
    gtk_widget_set_hexpand(self->time, true);
    gtk_label_set_width_chars(GTK_LABEL(self->time), TIME_CHARS);
    gtk_label_set_xalign(GTK_LABEL(self->time), 1);
    self->clock = part(self, "", "clock");
    gtk_accessible_update_state(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_STATE_HIDDEN, true, -1);
}
GtkWidget *popup_header_new(void) {
    return g_object_new(tl_popup_header_get_type(), NULL);
}
/* The widest clock: a two-digit day's last second in the current month, named in the
 * current locale. */
static void show_widest(TlPopupHeader *self) {
    char text[CLOCK_BYTES];
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *sample =
        now == NULL ? NULL
                    : g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
                                            WIDEST_DAY, LAST_HOUR, LAST_MINUTE, LAST_SECOND);
    text[0] = 0;
    if (sample != NULL)
        clock_text(sample, text, sizeof(text));
    gtk_label_set_text(GTK_LABEL(self->clock), text);
    if (sample != NULL)
        g_date_time_unref(sample);
    if (now != NULL)
        g_date_time_unref(now);
}
void popup_header_set_widest(GtkWidget *header, bool widest) {
    TlPopupHeader *self = (TlPopupHeader *)header;
    if (widest && self->saved_clock == NULL) {
        self->saved_clock = g_strdup(gtk_label_get_text(GTK_LABEL(self->clock)));
        show_widest(self);
    } else if (!widest && self->saved_clock != NULL) {
        gtk_label_set_text(GTK_LABEL(self->clock), self->saved_clock);
        g_clear_pointer(&self->saved_clock, g_free);
    }
}
/* "<0.1ms", "0.4ms", "3.2ms", "12ms", "140ms" or "1.2s": rounded, at most TIME_CHARS. */
static void format_time(uint64_t micros, char *text, size_t size) {
    uint64_t tenths = (micros + US_PER_TENTH_MS / 2) / US_PER_TENTH_MS;
    int written = 0;
    if (tenths == 0)
        written = snprintf(text, size, "<0.1ms");
    else if (tenths < TENTHS_SHOWN)
        written = snprintf(text, size, "%" PRIu64 ".%" PRIu64 "ms", tenths / 10, tenths % 10);
    else if (micros < WHOLE_MS_BELOW)
        written = snprintf(text, size, "%" PRIu64 "ms", (micros + US_PER_MS / 2) / US_PER_MS);
    else {
        uint64_t seconds = (micros + US_PER_TENTH_SECOND / 2) / US_PER_TENTH_SECOND;
        written = snprintf(text, size, "%" PRIu64 ".%" PRIu64 "s", seconds / 10, seconds % 10);
    }
    if (written < 0 || (size_t)written >= size)
        text[0] = 0;
}
void popup_header_set_search_time(GtkWidget *header, uint64_t micros) {
    TlPopupHeader *self = (TlPopupHeader *)header;
    char text[TIME_BYTES] = "";
    if (micros != POPUP_SEARCH_TIME_UNKNOWN)
        format_time(micros, text, sizeof(text));
    gtk_label_set_text(GTK_LABEL(self->time), text);
}
void popup_header_show_search_time(GtkWidget *header, bool shown) {
    gtk_widget_set_visible(((TlPopupHeader *)header)->time, shown);
}
const char *popup_header_search_time(GtkWidget *header) {
    return gtk_label_get_text(GTK_LABEL(((TlPopupHeader *)header)->time));
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
