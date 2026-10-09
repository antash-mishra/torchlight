/* Gliding selection highlight: one child plus a highlight allocated beneath it on the frame clock.
 */
#include "selection_track.h"
/* Long enough to read as motion, short enough that arrow-key repeat never lags. */
#define GLIDE_US 130000
typedef struct {
    GtkWidget parent;
    GtkWidget *child, *highlight, *target;
    graphene_rect_t from, to, current;
    gint64 started;
    guint tick;
    bool shown, glide;
} TlSelectionTrack;
typedef struct {
    GtkWidgetClass parent;
} TlSelectionTrackClass;
G_DEFINE_TYPE(TlSelectionTrack, tl_selection_track, GTK_TYPE_WIDGET)
static float ease_out(float progress) {
    float remaining = 1.0F - progress;
    return 1.0F - remaining * remaining * remaining;
}
static void stop_glide(TlSelectionTrack *self) {
    if (self->tick != 0)
        gtk_widget_remove_tick_callback(GTK_WIDGET(self), self->tick);
    self->tick = 0;
}
static gboolean glide_step(GtkWidget *widget, GdkFrameClock *clock, gpointer context) {
    (void)context;
    TlSelectionTrack *self = (TlSelectionTrack *)widget;
    gint64 elapsed = gdk_frame_clock_get_frame_time(clock) - self->started;
    if (elapsed >= GLIDE_US) {
        self->current = self->to;
        self->tick = 0;
        gtk_widget_queue_allocate(widget);
        return G_SOURCE_REMOVE;
    }
    float eased = ease_out((float)elapsed / (float)GLIDE_US);
    graphene_rect_interpolate(&self->from, &self->to, (double)eased, &self->current);
    gtk_widget_queue_allocate(widget);
    return G_SOURCE_CONTINUE;
}
static bool animations_enabled(GtkWidget *widget) {
    gboolean enabled = false;
    g_object_get(gtk_widget_get_settings(widget), "gtk-enable-animations", &enabled, NULL);
    return enabled != 0;
}
/* Called during allocation, when the target's bounds are finally known. */
static void retarget(TlSelectionTrack *self, const graphene_rect_t *bounds) {
    /* A glide is consumed by one move; later relayouts (resize, rows reflow) jump. */
    bool glide = self->glide;
    self->glide = false;
    if (graphene_rect_equal(bounds, &self->to) && self->shown)
        return;
    bool animate = glide && self->shown && animations_enabled(GTK_WIDGET(self));
    GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(self));
    self->to = *bounds;
    if (!animate || clock == NULL) {
        stop_glide(self);
        self->current = *bounds;
        return;
    }
    self->from = self->current;
    self->started = gdk_frame_clock_get_frame_time(clock);
    if (self->tick == 0)
        self->tick = gtk_widget_add_tick_callback(GTK_WIDGET(self), glide_step, NULL, NULL);
}
static void measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
                    int *natural, int *minimum_baseline, int *natural_baseline) {
    /* Only the child determines size; the highlight always fits inside it. */
    gtk_widget_measure(((TlSelectionTrack *)widget)->child, orientation, for_size, minimum, natural,
                       minimum_baseline, natural_baseline);
}
static void allocate(GtkWidget *widget, int width, int height, int baseline) {
    TlSelectionTrack *self = (TlSelectionTrack *)widget;
    gtk_widget_allocate(self->child, width, height, baseline, NULL);
    graphene_rect_t bounds;
    bool found = self->target != NULL && gtk_widget_get_parent(self->target) != NULL &&
                 gtk_widget_compute_bounds(self->target, widget, &bounds);
    if (found)
        retarget(self, &bounds);
    else
        stop_glide(self);
    self->shown = found;
    gtk_widget_set_child_visible(self->highlight, found);
    if (!found)
        return;
    graphene_point_t origin = self->current.origin;
    gtk_widget_allocate(self->highlight, (int)self->current.size.width,
                        (int)self->current.size.height, -1, gsk_transform_translate(NULL, &origin));
}
static void snapshot(GtkWidget *widget, GtkSnapshot *snapshot_value) {
    TlSelectionTrack *self = (TlSelectionTrack *)widget;
    gtk_widget_snapshot_child(widget, self->highlight, snapshot_value);
    gtk_widget_snapshot_child(widget, self->child, snapshot_value);
}
static void dispose(GObject *object) {
    TlSelectionTrack *self = (TlSelectionTrack *)object;
    stop_glide(self);
    g_clear_object(&self->target);
    g_clear_pointer(&self->highlight, gtk_widget_unparent);
    g_clear_pointer(&self->child, gtk_widget_unparent);
    G_OBJECT_CLASS(tl_selection_track_parent_class)->dispose(object);
}
static void tl_selection_track_class_init(TlSelectionTrackClass *class) {
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(class);
    widget->measure = measure;
    widget->size_allocate = allocate;
    widget->snapshot = snapshot;
    gtk_widget_class_set_css_name(widget, "selection-track");
    G_OBJECT_CLASS(class)->dispose = dispose;
}
static void tl_selection_track_init(TlSelectionTrack *self) {
    self->highlight = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->highlight, "selection-highlight");
    gtk_widget_set_name(self->highlight, "popup-highlight");
    GtkWidget *keycap = gtk_label_new("Enter");
    gtk_widget_add_css_class(keycap, "keycap");
    gtk_widget_set_hexpand(keycap, true);
    gtk_widget_set_halign(keycap, GTK_ALIGN_END);
    gtk_widget_set_valign(keycap, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(self->highlight), keycap);
    gtk_widget_set_can_target(self->highlight, false);
    gtk_accessible_update_state(GTK_ACCESSIBLE(self->highlight), GTK_ACCESSIBLE_STATE_HIDDEN, true,
                                -1);
    gtk_widget_set_parent(self->highlight, GTK_WIDGET(self));
    gtk_widget_set_child_visible(self->highlight, false);
}
GtkWidget *popup_selection_track_new(GtkWidget *child) {
    TlSelectionTrack *self = g_object_new(tl_selection_track_get_type(), NULL);
    self->child = child;
    gtk_widget_set_parent(child, GTK_WIDGET(self));
    return GTK_WIDGET(self);
}
void popup_selection_track_set_target(GtkWidget *track, GtkWidget *target, bool glide) {
    TlSelectionTrack *self = (TlSelectionTrack *)track;
    g_set_object(&self->target, target);
    self->glide = glide;
    if (target == NULL) {
        /* Hide now: an emptied list may be hidden and never allocated again. */
        stop_glide(self);
        self->shown = false;
        gtk_widget_set_child_visible(self->highlight, false);
    }
    gtk_widget_queue_allocate(track);
}
GtkWidget *popup_selection_track_get_target(GtkWidget *track) {
    return ((TlSelectionTrack *)track)->target;
}
GtkWidget *popup_selection_track_get_highlight(GtkWidget *track) {
    return ((TlSelectionTrack *)track)->highlight;
}
