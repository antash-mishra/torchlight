/* Idle strip: while the query is empty, the popup shows what the index is doing, each
 * fact behind a teletext colour block (ADR 0048). It keeps to one line: a large desktop
 * font drops the least important facts rather than widen the popup. */
#include "index_strip.h"
#include <inttypes.h>
#include <stdio.h>
#define FACTS 3
#define FACT_BYTES 64
#define STRIP_SPACING 22
#define FACT_SPACING 8
#define MS_PER_SECOND 1000
typedef struct {
    GtkWidget parent;
    GtkWidget *facts[FACTS], *blocks[FACTS], *labels[FACTS];
    /* Facts with something to say; fitting may still leave some of them out. */
    bool wanted[FACTS];
} TlPopupIndexStrip;
typedef struct {
    GtkWidgetClass parent;
} TlPopupIndexStripClass;
G_DEFINE_TYPE(TlPopupIndexStrip, tl_popup_index_strip, GTK_TYPE_WIDGET)
static const char *const COLOURS[] = {"red", "green", "yellow", "cyan"};
static void show_fact(TlPopupIndexStrip *self, size_t fact, const char *colour, const char *text) {
    for (size_t i = 0; i < G_N_ELEMENTS(COLOURS); i++)
        gtk_widget_remove_css_class(self->blocks[fact], COLOURS[i]);
    if (colour != NULL)
        gtk_widget_add_css_class(self->blocks[fact], colour);
    gtk_label_set_text(GTK_LABEL(self->labels[fact]), text != NULL ? text : "");
    self->wanted[fact] = text != NULL;
    gtk_widget_set_visible(self->facts[fact], text != NULL);
}
/* Thousands grouped with spaces, the Nordic way and the same in every locale: 34 477. */
static void grouped(size_t value, char *out, size_t size) {
    char digits[32];
    int length = snprintf(digits, sizeof(digits), "%zu", value);
    size_t at = 0;
    for (int i = 0; length > 0 && i < length && at + 2 < size; i++) {
        if (i != 0 && (length - i) % 3 == 0)
            out[at++] = ' ';
        out[at++] = digits[i];
    }
    out[at] = 0;
}
static void show_details(TlPopupIndexStrip *self, const tl_popup_index *index) {
    char number[32], text[FACT_BYTES];
    grouped(index->watches, number, sizeof(number));
    int written = snprintf(text, sizeof(text), "%s folders watched", number);
    show_fact(self, 1, "cyan", written > 0 && (size_t)written < sizeof(text) ? text : NULL);
    uint64_t ms = index->last_scan_ms;
    written = ms < MS_PER_SECOND
                  ? snprintf(text, sizeof(text), "Last scan %" PRIu64 " ms", ms)
                  : snprintf(text, sizeof(text), "Last scan %.1f s", (double)ms / MS_PER_SECOND);
    show_fact(self, 2, "yellow", written > 0 && (size_t)written < sizeof(text) ? text : NULL);
}
static void dispose(GObject *object) {
    for (GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(object)); child != NULL;
         child = gtk_widget_get_first_child(GTK_WIDGET(object)))
        gtk_widget_unparent(child);
    G_OBJECT_CLASS(tl_popup_index_strip_parent_class)->dispose(object);
}
static void tl_popup_index_strip_class_init(TlPopupIndexStripClass *class) {
    gtk_widget_class_set_layout_manager_type(GTK_WIDGET_CLASS(class), GTK_TYPE_BOX_LAYOUT);
    G_OBJECT_CLASS(class)->dispose = dispose;
}
static void tl_popup_index_strip_init(TlPopupIndexStrip *self) {
    gtk_widget_add_css_class(GTK_WIDGET(self), "index-strip");
    gtk_widget_set_name(GTK_WIDGET(self), "popup-index");
    gtk_box_layout_set_spacing(GTK_BOX_LAYOUT(gtk_widget_get_layout_manager(GTK_WIDGET(self))),
                               STRIP_SPACING);
    for (size_t i = 0; i < FACTS; i++) {
        self->facts[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, FACT_SPACING);
        self->blocks[i] = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(self->blocks[i], "block");
        gtk_widget_set_valign(self->blocks[i], GTK_ALIGN_CENTER);
        self->labels[i] = gtk_label_new(NULL);
        gtk_box_append(GTK_BOX(self->facts[i]), self->blocks[i]);
        gtk_box_append(GTK_BOX(self->facts[i]), self->labels[i]);
        gtk_widget_set_parent(self->facts[i], GTK_WIDGET(self));
    }
    popup_index_strip_set(GTK_WIDGET(self), &(tl_popup_index){0});
}
GtkWidget *popup_index_strip_new(void) {
    return g_object_new(tl_popup_index_strip_get_type(), NULL);
}
void popup_index_strip_set(GtkWidget *strip, const tl_popup_index *index) {
    TlPopupIndexStrip *self = (TlPopupIndexStrip *)strip;
    if (!index->known) {
        show_fact(self, 0, "yellow", "Checking the index");
        for (size_t i = 1; i < FACTS; i++)
            show_fact(self, i, NULL, NULL);
        return;
    }
    show_fact(self, 0,
              index->degraded   ? "red"
              : index->updating ? "yellow"
                                : "green",
              index->degraded   ? "Some folders are unavailable"
              : index->updating ? "Updating the index"
                                : "Index up to date");
    show_details(self, index);
}
void popup_index_strip_fit(GtkWidget *strip, int width) {
    TlPopupIndexStrip *self = (TlPopupIndexStrip *)strip;
    for (size_t i = 0; i < FACTS; i++)
        gtk_widget_set_visible(self->facts[i], self->wanted[i]);
    for (size_t i = FACTS - 1; i > 0; i--) {
        int minimum, natural;
        gtk_widget_measure(strip, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural, NULL, NULL);
        if (natural <= width)
            return;
        gtk_widget_set_visible(self->facts[i], false);
    }
}
const char *popup_index_strip_text(GtkWidget *strip, size_t fact) {
    if (fact >= FACTS)
        return NULL;
    return gtk_label_get_text(GTK_LABEL(((TlPopupIndexStrip *)strip)->labels[fact]));
}
bool popup_index_strip_shown(GtkWidget *strip, size_t fact) {
    return fact < FACTS && gtk_widget_get_visible(((TlPopupIndexStrip *)strip)->facts[fact]);
}
