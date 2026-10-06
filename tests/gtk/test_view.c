/* Native presentation regressions: real allocations, shaped caret pixels and timer cleanup. */
#include "../../ui/gtk/path_label.h"
#include "../../ui/gtk/view.h"
#include "../unit/test.h"
#include <math.h>
#include <string.h>
static void settle(int milliseconds) {
    gint64 deadline = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    do {
        while (g_main_context_iteration(NULL, false)) {
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
}
static GtkWidget *find_named(GtkWidget *parent, const char *name) {
    if (strcmp(gtk_widget_get_name(parent), name) == 0)
        return parent;
    for (GtkWidget *child = gtk_widget_get_first_child(parent); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_named(child, name);
        if (found != NULL)
            return found;
    }
    return NULL;
}
static graphene_rect_t bounds(GtkWidget *widget) {
    graphene_rect_t result;
    CHECK(gtk_widget_compute_bounds(widget, GTK_WIDGET(gtk_widget_get_root(widget)), &result));
    return result;
}
static int caret_pixels(tl_popup_widgets widgets, const char *artifact) {
    GtkSnapshot *snapshot = gtk_snapshot_new();
    GdkPaintable *paintable = gtk_widget_paintable_new(widgets.root);
    int width = (int)bounds(widgets.root).size.width,
        height = (int)bounds(widgets.root).size.height;
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    CHECK(node != NULL);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    gsk_render_node_draw(node, cr);
    cairo_surface_flush(surface);
    const char *capture = g_getenv("TORCHLIGHT_TEST_VIEW_CAPTURE");
    if (artifact != NULL && capture != NULL) {
        char *filename = g_strdup_printf("%s/native-%s.png", capture, artifact);
        CHECK(cairo_surface_write_to_png(surface, filename) == CAIRO_STATUS_SUCCESS);
        g_free(filename);
    }
    graphene_rect_t entry;
    CHECK(gtk_widget_compute_bounds(widgets.entry, widgets.root, &entry));
    unsigned char *bytes = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface), pixels = 0;
    int min_x = width, max_x = 0, min_y = height, max_y = 0;
    for (int y = (int)ceilf(entry.origin.y); y < (int)(entry.origin.y + entry.size.height); y++) {
        for (int x = (int)ceilf(entry.origin.x); x < (int)(entry.origin.x + entry.size.width);
             x++) {
            uint32_t color;
            memcpy(&color, bytes + y * stride + x * 4, sizeof(color));
            if ((color & 0xffffffU) != 0xb8d49dU)
                continue;
            pixels++;
            min_x = MIN(min_x, x);
            max_x = MAX(max_x, x);
            min_y = MIN(min_y, y);
            max_y = MAX(max_y, y);
        }
    }
    if (pixels != 0) {
        CHECK(max_x - min_x >= 10 && max_x - min_x <= 12);
        CHECK(max_y - min_y <= 2);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    return pixels;
}
static void check_path(const char *full, const char *expected) {
    GtkWidget *window = gtk_window_new(), *path = popup_path_label_new(full);
    g_object_ref_sink(window);
    gtk_window_set_decorated(GTK_WINDOW(window), false);
    gtk_window_set_resizable(GTK_WINDOW(window), false);
    gtk_window_set_child(GTK_WINDOW(window), path);
    PangoLayout *layout =
        gtk_widget_create_pango_layout(gtk_widget_get_first_child(path), expected);
    int width;
    pango_layout_get_pixel_size(layout, &width, NULL);
    gtk_window_set_default_size(GTK_WINDOW(window), width, 30);
    gtk_window_present(GTK_WINDOW(window));
    settle(80);
    CHECK(strcmp(gtk_label_get_text(GTK_LABEL(gtk_widget_get_first_child(path))), expected) == 0);
    CHECK(strcmp(gtk_widget_get_tooltip_text(path), full) == 0);
    g_object_unref(layout);
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);
}
static void test_paths(void) {
    check_path("~/workspace/torchlight/src/core", "~/…/torchlight/src/core");
    check_path("/very/long/ancestor/torchlight/src/core", "/…/torchlight/src/core");
    check_path("/src/core", "/src/core");
    char *home = g_build_filename(g_get_home_dir(), "workspace", "torchlight", "src", "core", NULL);
    check_path(home, "~/…/torchlight/src/core");
    g_free(home);
    check_path("/界界界界界界界界界界界界界界界界界界界界", "/界界…界界");
}
static void check_editing(tl_popup_widgets widgets) {
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "abc");
    gtk_editable_set_position(GTK_EDITABLE(widgets.entry), -1);
    settle(40);
    CHECK(caret_pixels(widgets, NULL) >= 12);
    gtk_editable_select_region(GTK_EDITABLE(widgets.entry), 0, -1);
    settle(40);
    CHECK(caret_pixels(widgets, NULL) == 0);
    gtk_editable_set_position(GTK_EDITABLE(widgets.entry), 0);
    settle(40);
    CHECK(caret_pixels(widgets, NULL) >= 12);
    GtkText *text = GTK_TEXT(gtk_editable_get_delegate(GTK_EDITABLE(widgets.entry)));
    g_signal_emit_by_name(text, "preedit-changed", "あ");
    settle(40);
    CHECK(gtk_widget_has_css_class(widgets.entry, "composing"));
    g_signal_emit_by_name(text, "preedit-changed", "");
    settle(40);
    CHECK(!gtk_widget_has_css_class(widgets.entry, "composing"));
    CHECK(caret_pixels(widgets, NULL) >= 12);
    char *paste = g_strnfill(300, 'x');
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), paste);
    gtk_editable_set_position(GTK_EDITABLE(widgets.entry), -1);
    settle(40);
    CHECK(caret_pixels(widgets, NULL) >= 12);
    g_free(paste);
}
static void check_feedback(tl_popup_view *view, tl_popup_widgets widgets) {
    GtkWidget *surface = find_named(widgets.root, "popup-search");
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "c");
    settle(400);
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "co");
    settle(400);
    CHECK(gtk_widget_has_css_class(surface, "typing"));
    settle(400);
    CHECK(!gtk_widget_has_css_class(surface, "typing"));
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "config");
    gtk_widget_set_visible(widgets.retry, true);
    gtk_widget_grab_focus(widgets.retry);
    settle(40);
    CHECK(!gtk_widget_has_css_class(surface, "typing"));
    gtk_widget_grab_focus(widgets.entry);
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "config.c");
    CHECK(gtk_widget_has_css_class(surface, "typing"));
    popup_view_set_active(view, false);
    settle(40);
    CHECK(!gtk_widget_has_css_class(surface, "typing"));
    CHECK(caret_pixels(widgets, NULL) == 0);
}
static void check_results(tl_popup_view *view, tl_popup_widgets widgets) {
    graphene_rect_t before = bounds(widgets.entry), after;
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "config");
    gtk_editable_set_position(GTK_EDITABLE(widgets.entry), -1);
    tl_popup_row row = {.id = 1};
    g_strlcpy(row.name, "config.c", sizeof(row.name));
    g_strlcpy(row.display, "~/workspace/torchlight/src/core/config.c", sizeof(row.display));
    gtk_list_box_append(GTK_LIST_BOX(widgets.list), popup_view_result(&row));
    gtk_list_box_select_row(GTK_LIST_BOX(widgets.list),
                            gtk_list_box_get_row_at_index(GTK_LIST_BOX(widgets.list), 0));
    popup_view_set_results(view, 1);
    popup_view_set_status(view, "1 result");
    settle(60);
    after = bounds(widgets.entry);
    CHECK(before.origin.x == after.origin.x && before.origin.y == after.origin.y &&
          before.size.width == after.size.width);
    CHECK(gtk_widget_get_visible(widgets.scroll));
    CHECK(gtk_widget_get_visible(find_named(widgets.root, "popup-footer")));
    CHECK(caret_pixels(widgets, "results") >= 12);
    popup_view_set_results(view, 0);
    gtk_list_box_remove_all(GTK_LIST_BOX(widgets.list));
    popup_view_set_status(view, "No matches. Try a name or part of its folder path.");
    settle(60);
    CHECK((int)bounds(widgets.root).size.height == 116);
}
static void test_view(void) {
    GtkWidget *window = gtk_window_new();
    g_object_ref_sink(window);
    gtk_window_set_decorated(GTK_WINDOW(window), false);
    gtk_window_set_resizable(GTK_WINDOW(window), false);
    gtk_widget_add_css_class(window, "torchlight");
    gtk_window_set_default_size(GTK_WINDOW(window), 680, -1);
    tl_popup_view *view = NULL;
    CHECK(popup_view_create(NULL, &view) == TL_INVALID && view == NULL);
    CHECK(popup_view_create(window, &view) == TL_OK);
    tl_popup_widgets widgets = popup_view_widgets(view);
    GtkSettings *settings = gtk_widget_get_settings(window);
    g_object_set(settings, "gtk-enable-animations", false, NULL);
    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(widgets.entry);
    popup_view_set_active(view, true);
    settle(100);
    CHECK((int)bounds(widgets.root).size.height == 116);
    CHECK(caret_pixels(widgets, "empty") >= 12);
    CHECK(!gtk_widget_get_visible(widgets.scroll));
    CHECK(!gtk_widget_get_visible(find_named(widgets.root, "popup-footer")));
    CHECK(gtk_entry_get_icon_storage_type(GTK_ENTRY(widgets.entry), GTK_ENTRY_ICON_SECONDARY) ==
          GTK_IMAGE_EMPTY);
    check_editing(widgets);
    check_results(view, widgets);
    check_feedback(view, widgets);
    popup_view_set_active(view, true);
    gtk_widget_grab_focus(widgets.entry);
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "destroy");
    gtk_window_destroy(GTK_WINDOW(window));
    popup_view_destroy(view);
    g_object_unref(window);
    settle(800); /* Destroying with a pending hold must never call freed context. */
}
int main(void) {
    gtk_init();
    test_paths();
    test_view();
    puts(
        "Native view: path fitting, underscore pixels, selection/IME, geometry and timers passed.");
    return 0;
}
