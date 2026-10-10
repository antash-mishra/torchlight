/* Allocate a path label using Pango measurements, preserving distinguishing and final folders. */
#include "path_label.h"
#include "torchlight/popup.h"
#include <stdbool.h>
#include <string.h>
typedef struct {
    GtkWidget parent;
    GtkWidget *label;
    /* The short form and the byte length of its distinguishing head (0 for none). */
    char *shown;
    size_t keep;
} TlPathLabel;
typedef struct {
    GtkWidgetClass parent;
} TlPathLabelClass;
G_DEFINE_TYPE(TlPathLabel, tl_path_label, GTK_TYPE_WIDGET)
static bool fits(PangoLayout *layout, const char *text, int width) {
    int measured;
    pango_layout_set_text(layout, text, -1);
    pango_layout_get_pixel_size(layout, &measured, NULL);
    return measured <= width;
}
/* Keep a distinguishing head, then as many trailing folders as fit after it. */
static char *shorten_kept(PangoLayout *layout, const char *path, size_t keep, int width) {
    char *head = g_strndup(path, keep);
    for (const char *slash = strchr(path + keep + 1, '/'); slash != NULL;
         slash = strchr(slash + 1, '/')) {
        char *candidate = g_strconcat(head, "/…/", slash + 1, NULL);
        if (fits(layout, candidate, width)) {
            g_free(head);
            return candidate;
        }
        g_free(candidate);
    }
    g_free(head);
    return NULL;
}
static char *shorten(PangoLayout *layout, const char *path, size_t keep, int width) {
    if (fits(layout, path, width))
        return g_strdup(path);
    if (keep != 0 && path[keep] == '/') {
        char *kept = shorten_kept(layout, path, keep, width);
        if (kept != NULL)
            return kept;
    }
    const char *root = g_str_has_prefix(path, "~/") ? "~/" : path[0] == '/' ? "/" : "";
    const char *tail = path + strlen(root);
    for (const char *slash = strchr(tail, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        char *candidate = g_strconcat(root, "…/", slash + 1, NULL);
        if (fits(layout, candidate, width))
            return candidate;
        g_free(candidate);
    }
    const char *last = strrchr(tail, '/');
    const char *folder = last == NULL ? tail : last + 1;
    char *prefix = g_strconcat(root, last == NULL ? "" : "…/", NULL);
    glong length = g_utf8_strlen(folder, -1);
    glong lower = 0, upper = length - 1;
    char *best = g_strdup("…");
    /* Logarithmic probes bound work for a pasted or indexed giant folder name. */
    while (lower < upper) {
        glong count = lower + (upper - lower + 1) / 2;
        char *left =
            g_strndup(folder, (gsize)(g_utf8_offset_to_pointer(folder, (count + 1) / 2) - folder));
        const char *right = g_utf8_offset_to_pointer(folder, length - count / 2);
        char *candidate = g_strconcat(prefix, left, "…", right, NULL);
        g_free(left);
        if (fits(layout, candidate, width)) {
            g_free(best);
            best = candidate;
            lower = count;
        } else {
            g_free(candidate);
            upper = count - 1;
        }
    }

    g_free(prefix);
    return best;
}
/* Number of the folder that ends at byte keep of path, counted after root;
 * count when keep is not past the root or not at a folder boundary. */
static guint kept_folder(const char *path, size_t root, size_t keep, guint count) {
    if (keep <= root || (path[keep] != '/' && path[keep] != 0))
        return count;
    guint folder = 0;
    for (size_t i = root; i < keep; i++)
        folder += path[i] == '/';
    return folder;
}
/* The short form: at most the last POPUP_SHORT_FOLDERS folders, or the distinguishing
 * folder and the last one. keep_out is its distinguishing head's length. */
static char *short_form(const char *path, size_t keep, size_t *keep_out) {
    *keep_out = 0;
    const char *root = g_str_has_prefix(path, "~/") ? "~/" : path[0] == '/' ? "/" : "";
    const char *tail = path + strlen(root);
    char **folders = g_strsplit(tail, "/", -1);
    guint count = g_strv_length(folders);
    guint kept = kept_folder(path, strlen(root), keep, count);
    char *result = NULL;
    if (tail[0] == 0 || count <= POPUP_SHORT_FOLDERS)
        result = g_strdup(path);
    else if (kept + POPUP_SHORT_FOLDERS >= count) /* none, or already among the last folders */
        result = g_strconcat(root, "…/", folders[count - 2], "/", folders[count - 1], NULL);
    else {
        char *head = g_strconcat(root, kept == 0 ? "" : "…/", folders[kept], NULL);
        *keep_out = strlen(head);
        result = g_strconcat(head, "/…/", folders[count - 1], NULL);
        g_free(head);
    }
    g_strfreev(folders);
    return result;
}
static void measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
                    int *natural, int *minimum_baseline, int *natural_baseline) {
    TlPathLabel *self = (TlPathLabel *)widget;
    gtk_widget_measure(self->label, orientation, for_size, minimum, natural, minimum_baseline,
                       natural_baseline);
    /* Paths must never widen the fixed popup; the name determines its column. */
    if (orientation == GTK_ORIENTATION_HORIZONTAL)
        *minimum = *natural = 0;
}
static void allocate(GtkWidget *widget, int width, int height, int baseline) {
    TlPathLabel *self = (TlPathLabel *)widget;
    PangoLayout *layout = gtk_widget_create_pango_layout(self->label, NULL);
    char *text = shorten(layout, self->shown, self->keep, width);
    if (strcmp(text, gtk_label_get_text(GTK_LABEL(self->label))) != 0)
        gtk_label_set_text(GTK_LABEL(self->label), text);
    g_free(text);
    g_object_unref(layout);
    gtk_widget_allocate(self->label, width, height, baseline, NULL);
}
static void snapshot(GtkWidget *widget, GtkSnapshot *snapshot_value) {
    gtk_widget_snapshot_child(widget, ((TlPathLabel *)widget)->label, snapshot_value);
}
static void dispose(GObject *object) {
    TlPathLabel *self = (TlPathLabel *)object;
    if (self->label != NULL) {
        gtk_widget_unparent(self->label);
        self->label = NULL;
    }
    G_OBJECT_CLASS(tl_path_label_parent_class)->dispose(object);
}
static void finalize(GObject *object) {
    g_free(((TlPathLabel *)object)->shown);
    G_OBJECT_CLASS(tl_path_label_parent_class)->finalize(object);
}
static void tl_path_label_class_init(TlPathLabelClass *class) {
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(class);
    widget->measure = measure;
    widget->size_allocate = allocate;
    widget->snapshot = snapshot;
    gtk_widget_class_set_css_name(widget, "path-label");
    gtk_widget_class_set_accessible_role(widget, GTK_ACCESSIBLE_ROLE_LABEL);
    G_OBJECT_CLASS(class)->dispose = dispose;
    G_OBJECT_CLASS(class)->finalize = finalize;
}
static void tl_path_label_init(TlPathLabel *self) {
    self->label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(self->label), 1);
    gtk_label_set_ellipsize(GTK_LABEL(self->label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(self->label, "result-detail");
    gtk_widget_set_parent(self->label, GTK_WIDGET(self));
    /* Announce only the parent's full path, never the shortened visual child. */
    gtk_accessible_update_state(GTK_ACCESSIBLE(self->label), GTK_ACCESSIBLE_STATE_HIDDEN, true, -1);
    gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}
GtkWidget *popup_path_label_new(const char *path, size_t keep) {
    TlPathLabel *self = g_object_new(tl_path_label_get_type(), NULL);
    const char *home = g_get_home_dir();
    size_t length = strlen(home);
    bool in_home = g_str_has_prefix(path, home) && (path[length] == '/' || path[length] == 0);
    char *full = in_home ? g_strconcat("~", path + length, NULL) : g_strdup(path);
    /* keep counts bytes of the original path; "~" stands in for the home bytes. */
    size_t kept = keep < strlen(path) ? keep : 0;
    if (in_home)
        kept = kept > length ? kept - length + 1 : 0;
    self->shown = short_form(full, kept, &self->keep);
    g_free(full);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self), path);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self), GTK_ACCESSIBLE_PROPERTY_LABEL, path, -1);
    return GTK_WIDGET(self);
}
