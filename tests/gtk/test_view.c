/* Native presentation regressions: real allocations, gliding selection, motion timers,
 * Tekst's header, idle strip and Fastext footer, and paths. */
#include "../../ui/gtk/header.h"
#include "../../ui/gtk/index_strip.h"
#include "../../ui/gtk/path_label.h"
#include "../../ui/gtk/view.h"
#include "../unit/test.h"
#include <string.h>
#define SURFACE_WIDTH 680
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
/* Render the popup window, shadow included; with TORCHLIGHT_TEST_VIEW_CAPTURE set, save
 * it for the docs. */
static void capture(tl_popup_widgets widgets, const char *artifact) {
    const char *directory = g_getenv("TORCHLIGHT_TEST_VIEW_CAPTURE");
    GtkWidget *window = GTK_WIDGET(gtk_widget_get_root(widgets.root));
    int width = gtk_widget_get_width(window), height = gtk_widget_get_height(window);
    GtkSnapshot *snapshot = gtk_snapshot_new();
    GdkPaintable *paintable = gtk_widget_paintable_new(window);
    gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), width, height);
    GskRenderNode *node = gtk_snapshot_free_to_node(snapshot);
    CHECK(node != NULL);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cr = cairo_create(surface);
    gsk_render_node_draw(node, cr);
    cairo_surface_flush(surface);
    if (directory != NULL) {
        char *filename = g_strdup_printf("%s/native-%s.png", directory, artifact);
        CHECK(cairo_surface_write_to_png(surface, filename) == CAIRO_STATUS_SUCCESS);
        g_free(filename);
    }
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    gsk_render_node_unref(node);
    g_object_unref(paintable);
}
static void check_path(const char *full, size_t keep, const char *expected) {
    GtkWidget *window = gtk_window_new(), *path = popup_path_label_new(full, keep);
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
/* Folders show short: home as ~, then at most the last two folders. */
static void test_paths(void) {
    check_path("~/workspace/torchlight/src/core", 0, "~/…/src/core");
    check_path("/very/long/ancestor/torchlight/src/core", 0, "/…/src/core");
    check_path("/src/core", 0, "/src/core");
    char *home = g_build_filename(g_get_home_dir(), "workspace", "torchlight", "src", "core", NULL);
    check_path(home, 0, "~/…/src/core");
    char *shallow = g_build_filename(g_get_home_dir(), "Documents", "notes", NULL);
    check_path(shallow, 0, "~/Documents/notes");
    check_path(g_get_home_dir(), 0, "~");
    g_free(shallow);
    g_free(home);
    check_path("/界界界界界界界界界界界界界界界界界界界界", 0, "/界界…界界");
    /* Same-named results keep the folder that tells them apart (here, an NDK version). */
    const char *ndk = "/opt/Sdk/ndk/27.0.12077973/toolchains/llvm/usr/include/c++/v1";
    check_path(ndk, strlen("/opt/Sdk/ndk/27.0.12077973"), "/…/27.0.12077973/…/v1");
    char *in_home = g_build_filename(g_get_home_dir(), "Android", "ndk", "27.1.12297006",
                                     "toolchains", "llvm", "include", "c++", "v1", NULL);
    size_t version = strlen(in_home) - strlen("/toolchains/llvm/include/c++/v1");
    check_path(in_home, version, "~/…/27.1.12297006/…/v1");
    g_free(in_home);
    /* Out-of-range or non-boundary keeps fall back to ordinary shortening. */
    check_path("/very/long/ancestor/torchlight/src/core", 999, "/…/src/core");
    /* A distinguishing folder among the last two needs no extra room. */
    check_path("/opt/ndk/27.0/include", strlen("/opt/ndk/27.0"), "/…/27.0/include");
}
static GtkWidget *result_widget(tl_popup_row *row, tl_popup_entrance entrance, size_t index) {
    return popup_view_result(row, 0, entrance, index);
}
static const char *icon_name(GtkWidget *result) {
    GIcon *icon = gtk_image_get_gicon(GTK_IMAGE(gtk_widget_get_first_child(result)));
    CHECK(G_IS_THEMED_ICON(icon));
    return g_themed_icon_get_names(G_THEMED_ICON(icon))[0];
}
static void test_rows(void) {
    tl_popup_row row = {.id = 1};
    g_strlcpy(row.name, "config.c", sizeof(row.name));
    g_strlcpy(row.display, "/src/core/config.c", sizeof(row.display));
    GtkWidget *cascade = g_object_ref_sink(result_widget(&row, POPUP_ENTER_CASCADE, 9));
    CHECK(gtk_widget_has_css_class(cascade, "enter-7") &&
          gtk_widget_has_css_class(cascade, "kind-file"));
    CHECK(strcmp(icon_name(cascade), "text-x-csrc") == 0);
    GtkWidget *fresh = g_object_ref_sink(result_widget(&row, POPUP_ENTER_FADE, 0));
    CHECK(gtk_widget_has_css_class(fresh, "fresh") && !gtk_widget_has_css_class(fresh, "enter-0"));
    /* An extensionless name cannot be placed by its name, so it keeps a generic icon. */
    g_strlcpy(row.name, "set", sizeof(row.name));
    GtkWidget *plain = g_object_ref_sink(result_widget(&row, POPUP_ENTER_NONE, 0));
    CHECK(!gtk_widget_has_css_class(plain, "fresh") && !gtk_widget_has_css_class(plain, "enter-0"));
    CHECK(strcmp(icon_name(plain), "text-x-generic") == 0);
    /* One line: name, then the folder filling the rest; the row's tooltip is the full path. */
    GtkWidget *location = gtk_widget_get_last_child(plain);
    CHECK(strcmp(gtk_widget_get_css_name(location), "path-label") == 0);
    CHECK(gtk_widget_get_hexpand(location) &&
          strcmp(gtk_widget_get_tooltip_text(plain), "/src/core/config.c") == 0);
    row.folder = true;
    GtkWidget *folder = g_object_ref_sink(result_widget(&row, POPUP_ENTER_NONE, 0));
    CHECK(strcmp(icon_name(folder), "folder") == 0 &&
          gtk_widget_has_css_class(folder, "kind-folder"));
    g_object_unref(cascade);
    g_object_unref(fresh);
    g_object_unref(plain);
    g_object_unref(folder);
}
static const char *label_text(GtkWidget *widget, const char *class) {
    if (GTK_IS_LABEL(widget) && gtk_widget_has_css_class(widget, class))
        return gtk_label_get_text(GTK_LABEL(widget));
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        const char *text = label_text(child, class);
        if (text != NULL)
            return text;
    }
    return NULL;
}
/* Application rows are plain; their children are compact rows with their own icons. */
static void test_children(void) {
    tl_popup_row app = {.id = 1, .application = true};
    g_strlcpy(app.name, "Google Chrome", sizeof(app.name));
    g_strlcpy(app.display, "/apps/google-chrome.desktop", sizeof(app.display));
    g_strlcpy(app.icon, "google-chrome", sizeof(app.icon));
    GtkWidget *plain = g_object_ref_sink(popup_view_result(&app, 0, POPUP_ENTER_NONE, 0));
    /* An application row is its icon and name alone: no subtitle, count or tooltip. */
    CHECK(label_text(plain, "result-detail") == NULL && gtk_widget_get_tooltip_text(plain) == NULL);
    CHECK(gtk_widget_has_css_class(plain, "kind-app"));
    app.settings = true;
    GtkWidget *panel = g_object_ref_sink(popup_view_result(&app, 0, POPUP_ENTER_NONE, 0));
    CHECK(gtk_widget_has_css_class(panel, "kind-setting") &&
          !gtk_widget_has_css_class(panel, "kind-app"));
    g_object_unref(panel);
    app.settings = false;
    GtkWidget *name = gtk_widget_get_last_child(plain);
    CHECK(GTK_IS_LABEL(name) && strcmp(gtk_label_get_text(GTK_LABEL(name)), "Google Chrome") == 0);
    GtkWidget *window = g_object_ref_sink(
        popup_view_child(&app, POPUP_ITEM_WINDOW, "Docs", POPUP_ENTER_CASCADE, 1));
    CHECK(GTK_IS_LIST_BOX_ROW(window) && gtk_widget_has_css_class(window, "child-row"));
    GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(window));
    CHECK(gtk_widget_has_css_class(box, "child") && gtk_widget_has_css_class(box, "enter-1"));
    CHECK(gtk_widget_has_css_class(box, "window") && !gtk_widget_has_css_class(box, "new-window"));
    CHECK(gtk_widget_has_css_class(box, "kind-window"));
    CHECK(strcmp(icon_name(box), "google-chrome") == 0);
    CHECK(strcmp(label_text(window, "result-name"), "Docs") == 0);
    GtkWidget *more = g_object_ref_sink(popup_view_child(
        &app, POPUP_ITEM_MORE_WINDOWS, "Show 3 more windows", POPUP_ENTER_FADE, 6));
    box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(more));
    CHECK(strcmp(icon_name(box), "view-more-symbolic") == 0 &&
          gtk_widget_has_css_class(box, "fresh") && gtk_widget_has_css_class(box, "more-windows"));
    CHECK(strcmp(label_text(more, "child-action"), "Show 3 more windows") == 0);
    GtkWidget *added = g_object_ref_sink(
        popup_view_child(&app, POPUP_ITEM_NEW_WINDOW, "New window", POPUP_ENTER_NONE, 7));
    box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(added));
    CHECK(strcmp(icon_name(box), "list-add-symbolic") == 0 &&
          gtk_widget_has_css_class(box, "new-window"));
    g_object_unref(plain);
    g_object_unref(window);
    g_object_unref(more);
    g_object_unref(added);
}
static void check_editing(tl_popup_widgets widgets) {
    /* The first show offers the plain prompt; later shows rotate through tips. */
    CHECK(strcmp(gtk_entry_get_placeholder_text(GTK_ENTRY(widgets.entry)),
                 "Search apps, files and settings") == 0);
    char *paste = g_strnfill(300, 'x');
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), paste);
    gtk_editable_set_position(GTK_EDITABLE(widgets.entry), -1);
    settle(40);
    /* An oversized paste stays editable without widening the fixed popup. */
    CHECK((int)bounds(widgets.root).size.width == SURFACE_WIDTH);
    CHECK(strlen(gtk_editable_get_text(GTK_EDITABLE(widgets.entry))) == 300);
    g_free(paste);
}
static void check_feedback(tl_popup_view *view, tl_popup_widgets widgets) {
    GtkWidget *field = find_named(widgets.root, "popup-search");
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "c");
    settle(400);
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "co");
    settle(400);
    CHECK(gtk_widget_has_css_class(field, "typing"));
    settle(400);
    CHECK(!gtk_widget_has_css_class(field, "typing"));
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "config");
    gtk_widget_set_visible(widgets.retry, true);
    gtk_widget_grab_focus(widgets.retry);
    settle(40);
    CHECK(!gtk_widget_has_css_class(field, "typing"));
    gtk_widget_grab_focus(widgets.entry);
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "config.c");
    CHECK(gtk_widget_has_css_class(field, "typing"));
    popup_view_set_active(view, false);
    settle(40);
    CHECK(!gtk_widget_has_css_class(field, "typing"));
    gtk_widget_set_visible(widgets.retry, false);
}
static void fill(tl_popup_view *view, tl_popup_widgets widgets, size_t count) {
    for (size_t i = 0; i < count; i++) {
        tl_popup_row row = {.id = i + 1};
        g_strlcpy(row.name, "config.c", sizeof(row.name));
        g_strlcpy(row.display, "~/workspace/torchlight/src/core/config.c", sizeof(row.display));
        gtk_list_box_append(GTK_LIST_BOX(widgets.list),
                            popup_view_result(&row, 0, POPUP_ENTER_CASCADE, i));
    }
    popup_view_set_results(view, count);
}
static GtkWidget *select_index(tl_popup_view *view, tl_popup_widgets widgets, int index,
                               bool glide) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(widgets.list), index);
    gtk_list_box_select_row(GTK_LIST_BOX(widgets.list), row);
    popup_view_select(view, GTK_WIDGET(row), glide);
    return GTK_WIDGET(row);
}
/* A label beneath widget showing exactly text, or NULL. */
static GtkWidget *label_with(GtkWidget *widget, const char *text) {
    if (GTK_IS_LABEL(widget) && strcmp(gtk_label_get_text(GTK_LABEL(widget)), text) == 0)
        return widget;
    for (GtkWidget *child = gtk_widget_get_first_child(widget); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = label_with(child, text);
        if (found != NULL)
            return found;
    }
    return NULL;
}
static void check_results(tl_popup_view *view, tl_popup_widgets widgets) {
    graphene_rect_t before = bounds(widgets.entry), after;
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "config");
    gtk_editable_set_position(GTK_EDITABLE(widgets.entry), -1);
    fill(view, widgets, 1);
    GtkWidget *row = select_index(view, widgets, 0, false);
    popup_view_set_status(view, "");
    settle(60);
    after = bounds(widgets.entry);
    CHECK(before.origin.x == after.origin.x && before.origin.y == after.origin.y &&
          before.size.width == after.size.width);
    CHECK(gtk_widget_get_visible(widgets.scroll));
    CHECK(gtk_widget_get_visible(find_named(widgets.root, "popup-footer")));
    GtkWidget *track = find_named(widgets.root, "popup-highlight");
    graphene_rect_t highlight = bounds(track);
    graphene_rect_t selected = bounds(row);
    CHECK(graphene_rect_equal(&highlight, &selected));
    /* The block's edge takes the selected row's kind; the header counts the results. */
    CHECK(gtk_widget_has_css_class(track, "kind-file") &&
          !gtk_widget_has_css_class(track, "kind-app"));
    CHECK(strcmp(label_text(find_named(widgets.root, "popup-header"), "result-count"),
                 "1 result") == 0);
    /* With a query, the idle strip gives way to the results. */
    CHECK(!gtk_widget_get_visible(find_named(widgets.root, "popup-index")));
    popup_view_set_copied(view);
    CHECK(label_with(widgets.root, "Copied") != NULL &&
          label_with(widgets.root, "Copy path") == NULL);
    settle(1300);
    CHECK(label_with(widgets.root, "Copy path") != NULL);
    popup_view_set_results(view, 0);
    gtk_list_box_remove_all(GTK_LIST_BOX(widgets.list));
    popup_view_select(view, NULL, false);
    popup_view_set_status(view, "No matches. Try a name or part of its folder path.");
    settle(60);
    /* No matches keeps the compact query line: the header and search area alone. */
    graphene_rect_t header = bounds(find_named(widgets.root, "popup-header"));
    graphene_rect_t search = bounds(find_named(widgets.root, "popup-search"));
    CHECK(bounds(widgets.root).size.height < header.size.height + search.size.height + 30);
    CHECK(strcmp(label_text(find_named(widgets.root, "popup-header"), "result-count"), "") == 0);
    CHECK(!gtk_widget_get_child_visible(find_named(widgets.root, "popup-highlight")));
}
/* The highlight glides through intermediate places, then rests on the new row. */
static void check_glide(tl_popup_view *view, tl_popup_widgets widgets) {
    fill(view, widgets, 4);
    GtkWidget *first = select_index(view, widgets, 0, false);
    settle(80);
    GtkWidget *highlight = find_named(widgets.root, "popup-highlight");
    float start = bounds(first).origin.y;
    CHECK(bounds(highlight).origin.y == start);
    GtkWidget *last = select_index(view, widgets, 3, true);
    float end = bounds(last).origin.y;
    bool between = false;
    for (int i = 0; i < 60; i++) {
        settle(5);
        float y = bounds(highlight).origin.y;
        between = between || (y > start && y < end);
    }
    CHECK(between);
    CHECK(bounds(highlight).origin.y == end);
    popup_view_set_launching(view, true);
    GtkWidget *result = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(last));
    CHECK(gtk_widget_has_css_class(highlight, "flash"));
    CHECK(gtk_widget_has_css_class(result, "launching"));
    popup_view_set_launching(view, false);
    CHECK(!gtk_widget_has_css_class(highlight, "flash"));
    CHECK(!gtk_widget_has_css_class(result, "launching"));
    popup_view_set_results(view, 0);
    gtk_list_box_remove_all(GTK_LIST_BOX(widgets.list));
    popup_view_select(view, NULL, false);
}
static void check_searching(tl_popup_view *view, tl_popup_widgets widgets) {
    GtkWidget *spinner = find_named(widgets.root, "popup-spinner");
    GtkStack *icons = GTK_STACK(gtk_widget_get_parent(spinner));
    popup_view_set_searching(view, true);
    CHECK(strcmp(gtk_stack_get_visible_child_name(icons), "spinner") == 0);
    CHECK(gtk_spinner_get_spinning(GTK_SPINNER(spinner)));
    popup_view_set_searching(view, false);
    CHECK(strcmp(gtk_stack_get_visible_child_name(icons), "search") == 0);
    CHECK(!gtk_spinner_get_spinning(GTK_SPINNER(spinner)));
}
/* Opening effects end on a timer; dismissal fades, then hides; showing cancels a hide. */
static void check_motion(tl_popup_view *view, tl_popup_widgets widgets, GtkWidget *window) {
    popup_view_set_active(view, true);
    CHECK(gtk_widget_has_css_class(widgets.root, "opening"));
    settle(800);
    CHECK(!gtk_widget_has_css_class(widgets.root, "opening"));
    popup_view_dismiss(view);
    CHECK(gtk_widget_get_visible(window) && gtk_widget_has_css_class(widgets.root, "closing"));
    settle(300);
    CHECK(!gtk_widget_get_visible(window) && !gtk_widget_has_css_class(widgets.root, "closing"));
    gtk_window_present(GTK_WINDOW(window));
    popup_view_set_active(view, true);
    settle(100);
    popup_view_dismiss(view);
    popup_view_set_active(view, false);
    gtk_window_present(GTK_WINDOW(window));
    popup_view_set_active(view, true);
    settle(300);
    CHECK(gtk_widget_get_visible(window) && !gtk_widget_has_css_class(widgets.root, "closing"));
}
/* A representative list for the docs capture: an application with a window and New
 * window, then two files and a folder; the beam rests on the first file. */
static void capture_results(tl_popup_view *view, tl_popup_widgets widgets) {
    tl_popup_row app = {.id = 1, .application = true};
    g_strlcpy(app.name, "Terminal", sizeof(app.name));
    g_strlcpy(app.icon, "utilities-terminal", sizeof(app.icon));
    const char *files[][2] = {
        {"terminal-notes.md", "~/workspace/torchlight/docs/terminal-notes.md"},
        {"terminal.c", "~/workspace/torchlight/ui/gtk/terminal.c"},
        {"terminals", "~/Pictures/terminals"}};
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "term");
    gtk_list_box_append(GTK_LIST_BOX(widgets.list),
                        popup_view_result(&app, 0, POPUP_ENTER_NONE, 0));
    gtk_list_box_append(
        GTK_LIST_BOX(widgets.list),
        popup_view_child(&app, POPUP_ITEM_WINDOW, "~/workspace/torchlight", POPUP_ENTER_NONE, 1));
    gtk_list_box_append(
        GTK_LIST_BOX(widgets.list),
        popup_view_child(&app, POPUP_ITEM_NEW_WINDOW, "New window", POPUP_ENTER_NONE, 2));
    for (size_t i = 0; i < G_N_ELEMENTS(files); i++) {
        tl_popup_row row = {.id = i + 2, .folder = i == 2};
        g_strlcpy(row.name, files[i][0], sizeof(row.name));
        g_strlcpy(row.display, files[i][1], sizeof(row.display));
        gtk_list_box_append(GTK_LIST_BOX(widgets.list),
                            popup_view_result(&row, 0, POPUP_ENTER_NONE, i + 3));
    }
    popup_view_set_results(view, G_N_ELEMENTS(files) + 1);
    select_index(view, widgets, 3, false);
    popup_view_set_status(view, "");
    settle(800);
    capture(widgets, "results");
    popup_view_set_results(view, 0);
    gtk_list_box_remove_all(GTK_LIST_BOX(widgets.list));
    popup_view_select(view, NULL, false);
}
/* With a compositor and a GPU renderer the surface sits inside a transparent shadow
 * margin, and the window grows by it; otherwise the surface fills the window. */
static void check_shadow(tl_popup_view *view, tl_popup_widgets widgets) {
    tl_popup_margins margins = popup_view_margins(view);
    bool shadow = gtk_widget_has_css_class(widgets.root, "shadowed");
    GtkWidget *window = GTK_WIDGET(gtk_widget_get_root(widgets.root));
    GdkDisplay *display = gtk_widget_get_display(window);
    GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
    /* Software rendering redraws blurred shadows on every resize, so it gets none. */
    CHECK(shadow == (gdk_display_is_composited(display) && gdk_display_is_rgba(display) &&
                     !GSK_IS_CAIRO_RENDERER(renderer) &&
                     !gtk_widget_has_css_class(widgets.root, "high-contrast")));
    if (shadow)
        CHECK(margins.left > 0 && margins.left == margins.right && margins.top > 0 &&
              margins.bottom > margins.top);
    else
        CHECK(margins.left == 0 && margins.right == 0 && margins.top == 0 && margins.bottom == 0);
    graphene_rect_t surface = bounds(widgets.root);
    CHECK((int)surface.origin.x == margins.left && (int)surface.origin.y == margins.top);
    CHECK(gtk_widget_get_width(window) == SURFACE_WIDTH + margins.left + margins.right);
}
static GtkWidget *new_window(void) {
    GtkWidget *window = gtk_window_new();
    g_object_ref_sink(window);
    gtk_window_set_decorated(GTK_WINDOW(window), false);
    gtk_window_set_resizable(GTK_WINDOW(window), false);
    gtk_widget_add_css_class(window, "torchlight");
    gtk_window_set_default_size(GTK_WINDOW(window), SURFACE_WIDTH, -1);
    return window;
}
/* The idle strip: unknown, then known states, and one line however large the font. */
static void test_index_strip(void) {
    GtkWidget *window = new_window(), *strip = popup_index_strip_new();
    gtk_window_set_child(GTK_WINDOW(window), strip);
    CHECK(strcmp(popup_index_strip_text(strip, 0), "Checking the index") == 0);
    CHECK(!popup_index_strip_shown(strip, 1) && !popup_index_strip_shown(strip, 2));
    tl_popup_index index = {.known = true, .watches = 34477, .last_scan_ms = 1205};
    popup_index_strip_set(strip, &index);
    popup_index_strip_fit(strip, 10000);
    CHECK(strcmp(popup_index_strip_text(strip, 0), "Index up to date") == 0);
    CHECK(strcmp(popup_index_strip_text(strip, 1), "34 477 folders watched") == 0);
    CHECK(strcmp(popup_index_strip_text(strip, 2), "Last scan 1.2 s") == 0);
    CHECK(popup_index_strip_shown(strip, 1) && popup_index_strip_shown(strip, 2));
    index.last_scan_ms = 840;
    index.updating = true;
    popup_index_strip_set(strip, &index);
    CHECK(strcmp(popup_index_strip_text(strip, 0), "Updating the index") == 0);
    CHECK(strcmp(popup_index_strip_text(strip, 2), "Last scan 840 ms") == 0);
    index.degraded = true;
    popup_index_strip_set(strip, &index);
    CHECK(strcmp(popup_index_strip_text(strip, 0), "Some folders are unavailable") == 0);
    /* Too narrow: the last scan goes first, then the folders; the state stays. */
    popup_index_strip_fit(strip, 1);
    CHECK(popup_index_strip_shown(strip, 0) && !popup_index_strip_shown(strip, 1) &&
          !popup_index_strip_shown(strip, 2));
    popup_index_strip_fit(strip, 10000);
    CHECK(popup_index_strip_shown(strip, 1) && popup_index_strip_shown(strip, 2));
    gtk_window_destroy(GTK_WINDOW(window));
    g_object_unref(window);
}
/* A forced dark theme loads the dark palette, as the desktop's dark theme would. */
static void test_forced_dark(void) {
    g_setenv("GTK_THEME", "Adwaita:dark", true);
    GtkWidget *window = new_window();
    tl_popup_view *view = NULL;
    CHECK(popup_view_create(window, &view) == TL_OK);
    tl_popup_widgets widgets = popup_view_widgets(view);
    CHECK(gtk_widget_has_css_class(widgets.root, "dark") &&
          !gtk_widget_has_css_class(widgets.root, "light"));
    gtk_window_destroy(GTK_WINDOW(window));
    popup_view_destroy(view);
    g_object_unref(window);
    g_unsetenv("GTK_THEME");
}
/* GTK_THEME overrides the theme setting without changing it; a forced high-contrast
 * theme must still drop the palette and the shadow. */
static void test_forced_contrast(void) {
    g_setenv("GTK_THEME", "HighContrast", true);
    GtkWidget *window = new_window();
    tl_popup_view *view = NULL;
    CHECK(popup_view_create(window, &view) == TL_OK);
    tl_popup_widgets widgets = popup_view_widgets(view);
    popup_view_configure(view, SURFACE_WIDTH, 800);
    tl_popup_margins margins = popup_view_margins(view);
    CHECK(gtk_widget_has_css_class(widgets.root, "high-contrast"));
    CHECK(!gtk_widget_has_css_class(widgets.root, "shadowed") && margins.left == 0 &&
          margins.bottom == 0);
    gtk_window_destroy(GTK_WINDOW(window));
    popup_view_destroy(view);
    g_object_unref(window);
    g_unsetenv("GTK_THEME");
}
static void test_view(void) {
    GtkWidget *window = new_window();
    tl_popup_view *view = NULL;
    CHECK(popup_view_create(NULL, &view) == TL_INVALID && view == NULL);
    CHECK(popup_view_create(window, &view) == TL_OK);
    tl_popup_widgets widgets = popup_view_widgets(view);
    /* As the launcher does: realize, fit hints and the viewport to the work area, then
     * size the window for the surface and its shadow margin. */
    gtk_widget_realize(window);
    popup_view_configure(view, SURFACE_WIDTH, 800);
    tl_popup_margins margins = popup_view_margins(view);
    gtk_window_set_default_size(GTK_WINDOW(window), SURFACE_WIDTH + margins.left + margins.right,
                                -1);
    GtkSettings *settings = gtk_widget_get_settings(window);
    g_object_set(settings, "gtk-enable-animations", false, NULL);
    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(widgets.entry);
    popup_view_set_active(view, true);
    CHECK(!gtk_widget_has_css_class(widgets.root, "opening"));
    settle(100);
    /* Empty: the header with its clock, the query line and the idle strip, nothing else. */
    GtkWidget *header = find_named(widgets.root, "popup-header");
    CHECK(gtk_widget_get_visible(header) && strchr(popup_header_clock(header), '/') != NULL);
    CHECK(gtk_widget_get_visible(find_named(widgets.root, "popup-index")));
    CHECK(gtk_widget_has_css_class(widgets.root, "dark") !=
          gtk_widget_has_css_class(widgets.root, "light"));
    popup_view_set_index(view,
                         &(tl_popup_index){.known = true, .watches = 34477, .last_scan_ms = 1205});
    check_shadow(view, widgets);
    settle(60); /* The new facts relayout the strip; a snapshot waits for allocation. */
    capture(widgets, "empty");
    CHECK(!gtk_widget_get_visible(widgets.scroll));
    CHECK(!gtk_widget_get_visible(find_named(widgets.root, "popup-footer")));
    CHECK(gtk_entry_get_icon_storage_type(GTK_ENTRY(widgets.entry), GTK_ENTRY_ICON_SECONDARY) ==
          GTK_IMAGE_EMPTY);
    check_editing(widgets);
    /* Each later show offers the next tip. */
    popup_view_set_active(view, false);
    popup_view_set_active(view, true);
    CHECK(strcmp(gtk_entry_get_placeholder_text(GTK_ENTRY(widgets.entry)),
                 "Try raedme: typos are fine") == 0);
    check_results(view, widgets);
    capture_results(view, widgets);
    check_feedback(view, widgets);
    check_searching(view, widgets);
    popup_view_dismiss(view);
    CHECK(!gtk_widget_get_visible(window)); /* Without animations, dismissal is immediate. */
    g_object_set(settings, "gtk-enable-animations", true, NULL);
    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(widgets.entry);
    check_glide(view, widgets);
    check_motion(view, widgets, window);
    gtk_widget_grab_focus(widgets.entry);
    gtk_editable_set_text(GTK_EDITABLE(widgets.entry), "destroy");
    popup_view_dismiss(view);
    gtk_window_destroy(GTK_WINDOW(window));
    popup_view_destroy(view);
    g_object_unref(window);
    settle(800); /* Destroying with pending holds and fades must never call freed context. */
}
int main(void) {
    gtk_init();
    test_paths();
    test_rows();
    test_children();
    test_index_strip();
    test_view();
    test_forced_dark();
    test_forced_contrast();
    puts("Native view: path fitting, icons, entrances, glide, motion timers, header, idle strip, "
         "palettes, shadow and geometry passed.");
    return 0;
}
