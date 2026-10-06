/* Shader-free GTK presentation, shaped underscore caret and bounded typing feedback. */
#include "view.h"
#include "path_label.h"
#include <string.h>
#define TYPING_IDLE_MS 700
#define CARET_BLINK_MS 550
#define CARET_WIDTH 12
struct popup_view {
    tl_popup_widgets widgets;
    GtkWidget *window, *surface, *footer, *message, *message_box, *caret, *text, *reveal;
    GtkSettings *settings;
    GdkDisplay *display;
    GtkCssProvider *css;
    guint typing_timer, caret_timer;
    gint64 caret_started;
    size_t results;
    bool active, composing, caret_on, animations;
};
static void remove_timer(guint *timer) {
    if (*timer != 0)
        g_source_remove(*timer);
    *timer = 0;
}
static bool empty(tl_popup_view *view) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(view->widgets.entry));
    for (; *text != 0; text = g_utf8_next_char(text))
        if (!g_unichar_isspace(g_utf8_get_char(text)))
            return false;
    return true;
}
static void update_message(tl_popup_view *view) {
    bool retry = gtk_widget_get_visible(view->widgets.retry);
    gtk_widget_set_visible(view->message_box, !empty(view) && (view->results == 0 || retry));
}
static void retry_changed(GObject *object, GParamSpec *parameter, gpointer context) {
    (void)object;
    (void)parameter;
    update_message(context);
}
static gboolean typing_finished(gpointer context) {
    tl_popup_view *view = context;
    view->typing_timer = 0;
    gtk_widget_remove_css_class(view->surface, "typing");
    return G_SOURCE_REMOVE;
}
static gboolean blink_caret(gpointer context) {
    tl_popup_view *view = context;
    int timeout = 10;
    g_object_get(view->settings, "gtk-cursor-blink-timeout", &timeout, NULL);
    /* Like GTK, stop blinking after inactivity rather than waking forever. */
    if (g_get_monotonic_time() - view->caret_started >= (gint64)timeout * G_USEC_PER_SEC) {
        view->caret_timer = 0;
        view->caret_on = true;
        gtk_widget_queue_draw(view->caret);
        return G_SOURCE_REMOVE;
    }
    view->caret_on = !view->caret_on;
    gtk_widget_queue_draw(view->caret);
    return G_SOURCE_CONTINUE;
}
static void reset_caret(tl_popup_view *view) {
    remove_timer(&view->caret_timer);
    view->caret_on = true;
    view->caret_started = g_get_monotonic_time();
    gboolean blink = true;
    int interval = CARET_BLINK_MS * 2;
    g_object_get(view->settings, "gtk-cursor-blink", &blink, "gtk-cursor-blink-time", &interval,
                 NULL);
    int start, end;
    bool selection =
        gtk_editable_get_selection_bounds(GTK_EDITABLE(view->widgets.entry), &start, &end);
    if (view->active && view->animations && blink && !view->composing && !selection &&
        gtk_widget_has_focus(view->text))
        view->caret_timer = g_timeout_add((guint)MAX(100, interval / 2), blink_caret, view);
    gtk_widget_queue_draw(view->caret);
}
static void editing_changed(GtkEditable *entry, gpointer context) {
    (void)entry;
    tl_popup_view *view = context;
    remove_timer(&view->typing_timer);
    if (view->active && gtk_widget_has_focus(view->text)) {
        gtk_widget_add_css_class(view->surface, "typing");
        view->typing_timer = g_timeout_add(TYPING_IDLE_MS, typing_finished, view);
    }
    reset_caret(view);
    update_message(view);
}
static void cursor_changed(GObject *object, GParamSpec *parameter, gpointer context) {
    (void)object;
    (void)parameter;
    tl_popup_view *view = context;
    if (!gtk_widget_has_focus(view->text)) {
        remove_timer(&view->typing_timer);
        gtk_widget_remove_css_class(view->surface, "typing");
    }
    reset_caret(view);
}
static void preedit_changed(GtkText *text, const char *preedit, gpointer context) {
    (void)text;
    tl_popup_view *view = context;
    view->composing = preedit[0] != 0;
    if (view->composing)
        gtk_widget_add_css_class(view->widgets.entry, "composing");
    else
        gtk_widget_remove_css_class(view->widgets.entry, "composing");
    reset_caret(view);
}
static void draw_caret(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer context) {
    tl_popup_view *view = context;
    int start, end;
    (void)area;
    (void)height;
    if (!view->active || !view->caret_on || view->composing || !gtk_widget_has_focus(view->text) ||
        gtk_editable_get_selection_bounds(GTK_EDITABLE(view->widgets.entry), &start, &end))
        return;
    int position = gtk_editable_get_position(GTK_EDITABLE(view->widgets.entry));
    graphene_rect_t cursor;
    gtk_text_compute_cursor_extents(GTK_TEXT(view->text), (gsize)MAX(0, position), &cursor, NULL);
    graphene_point_t origin = GRAPHENE_POINT_INIT(0, 0), offset;
    if (!gtk_widget_compute_point(view->text, view->caret, &origin, &offset))
        return;
    double x = CLAMP((double)(cursor.origin.x + offset.x), 0, MAX(0, width - CARET_WIDTH));
    double y = (double)(cursor.origin.y + cursor.size.height + offset.y) - 2;
    cairo_set_source_rgb(cr, 184.0 / 255.0, 212.0 / 255.0, 157.0 / 255.0);
    cairo_rectangle(cr, x, y, CARET_WIDTH, 2);
    cairo_fill(cr);
}
static GtkWidget *label(const char *text, const char *class) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_widget_add_css_class(widget, class);
    return widget;
}
static GtkWidget *create_entry(tl_popup_view *view) {
    GtkWidget *overlay = gtk_overlay_new();
    view->widgets.entry = gtk_entry_new();
    gtk_entry_set_has_frame(GTK_ENTRY(view->widgets.entry), false);
    gtk_entry_set_placeholder_text(GTK_ENTRY(view->widgets.entry), "Search");
    gtk_editable_set_width_chars(GTK_EDITABLE(view->widgets.entry), 1);
    gtk_widget_set_hexpand(overlay, true);
    gtk_widget_set_size_request(overlay, -1, 34);
    gtk_widget_add_css_class(view->widgets.entry, "query-entry");
    gtk_widget_set_name(view->widgets.entry, "popup-query");
    view->text = GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(view->widgets.entry)));
    const char *name = "Search apps, settings, files and folders";
    gtk_accessible_update_property(GTK_ACCESSIBLE(view->widgets.entry),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(view->text), GTK_ACCESSIBLE_PROPERTY_LABEL, name,
                                   -1);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), view->widgets.entry);
    view->caret = gtk_drawing_area_new();
    gtk_widget_set_name(view->caret, "popup-caret");
    gtk_widget_set_can_target(view->caret, false);
    gtk_accessible_update_state(GTK_ACCESSIBLE(view->caret), GTK_ACCESSIBLE_STATE_HIDDEN, true, -1);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(view->caret), draw_caret, view, NULL);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), view->caret);
    return overlay;
}
static GtkWidget *create_search(tl_popup_view *view) {
    GtkWidget *overlay = gtk_overlay_new();
    view->surface = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20);
    gtk_widget_add_css_class(view->surface, "search-surface");
    gtk_widget_set_name(view->surface, "popup-search");
    GtkWidget *identity = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(identity, "wordmark");
    gtk_box_append(GTK_BOX(identity), label("//", "brand-mark"));
    gtk_box_append(GTK_BOX(identity), label("torchlight", "wordmark"));
    gtk_box_append(GTK_BOX(view->surface), identity);
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 17);
    gtk_box_append(GTK_BOX(line), label("/", "prompt"));
    gtk_box_append(GTK_BOX(line), create_entry(view));
    gtk_box_append(GTK_BOX(view->surface), line);
    gtk_overlay_set_child(GTK_OVERLAY(overlay), view->surface);
    view->message_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(view->message_box, "inline-status");
    gtk_widget_set_valign(view->message_box, GTK_ALIGN_END);
    view->message = label("", "message");
    gtk_label_set_ellipsize(GTK_LABEL(view->message), PANGO_ELLIPSIZE_END);
    view->widgets.retry = gtk_button_new_with_label("Retry ↵");
    gtk_widget_set_name(view->widgets.retry, "popup-retry");
    gtk_widget_add_css_class(view->widgets.retry, "retry");
    gtk_widget_set_visible(view->widgets.retry, false);
    gtk_box_append(GTK_BOX(view->message_box), view->message);
    gtk_box_append(GTK_BOX(view->message_box), view->widgets.retry);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), view->message_box);
    gtk_widget_set_visible(view->message_box, false);
    return overlay;
}
static GtkWidget *create_footer(tl_popup_view *view) {
    view->footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    gtk_widget_add_css_class(view->footer, "footer");
    gtk_widget_set_name(view->footer, "popup-footer");
    view->widgets.status = label("", "status");
    gtk_widget_set_name(view->widgets.status, "popup-status");
    gtk_label_set_ellipsize(GTK_LABEL(view->widgets.status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(view->widgets.status, true);
    gtk_box_append(GTK_BOX(view->footer), view->widgets.status);
    gtk_box_append(GTK_BOX(view->footer), label("↑↓ select   ↵ open", "key-hints"));
    view->reveal = label("ctrl ↵ reveal", "key-hints");
    gtk_widget_add_css_class(view->reveal, "reveal-hint");
    gtk_box_append(GTK_BOX(view->footer), view->reveal);
    gtk_box_append(GTK_BOX(view->footer), label("esc close", "key-hints"));
    gtk_widget_set_visible(view->footer, false);
    return view->footer;
}
static void settings_changed(GObject *object, GParamSpec *parameter, gpointer context) {
    (void)object;
    (void)parameter;
    tl_popup_view *view = context;
    gboolean enabled;
    char *theme = NULL;
    g_object_get(view->settings, "gtk-enable-animations", &enabled, "gtk-theme-name", &theme, NULL);
    view->animations = enabled != 0;
    if (view->animations)
        gtk_widget_remove_css_class(view->widgets.root, "quiet-motion");
    else
        gtk_widget_add_css_class(view->widgets.root, "quiet-motion");
    if (theme != NULL && strstr(theme, "HighContrast") != NULL)
        gtk_widget_add_css_class(view->widgets.root, "high-contrast");
    else
        gtk_widget_remove_css_class(view->widgets.root, "high-contrast");
    g_free(theme);
    reset_caret(view);
}
static void build_content(tl_popup_view *view) {
    view->widgets.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(view->widgets.root);
    gtk_widget_add_css_class(view->widgets.root, "launcher-surface");
    gtk_widget_set_overflow(view->widgets.root, GTK_OVERFLOW_HIDDEN);
    view->settings = g_object_ref(gtk_widget_get_settings(view->window));
    gtk_box_append(GTK_BOX(view->widgets.root), create_search(view));
    view->widgets.list = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(view->widgets.list), true);
    gtk_widget_set_focusable(view->widgets.list, false);
    view->widgets.scroll = gtk_scrolled_window_new();
    gtk_widget_set_name(view->widgets.scroll, "popup-results");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->widgets.scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(view->widgets.scroll),
                                                     true);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(view->widgets.scroll),
                                               8 * 58 + 16);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->widgets.scroll), view->widgets.list);
    gtk_widget_set_visible(view->widgets.scroll, false);
    gtk_box_append(GTK_BOX(view->widgets.root), view->widgets.scroll);
    gtk_box_append(GTK_BOX(view->widgets.root), create_footer(view));
    gtk_window_set_child(GTK_WINDOW(view->window), view->widgets.root);
}
static void install_style(tl_popup_view *view) {
    view->display = g_object_ref(gtk_widget_get_display(view->window));
    view->css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(view->css, "/org/torchlight/launcher/quiet-system.css");
    gtk_style_context_add_provider_for_display(view->display, GTK_STYLE_PROVIDER(view->css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}
static void connect_signals(tl_popup_view *view) {
    g_signal_connect(view->widgets.entry, "changed", G_CALLBACK(editing_changed), view);
    for (const char **name = (const char *[]){"notify::cursor-position", "notify::selection-bound",
                                              "notify::scroll-offset", "notify::has-focus", NULL};
         *name != NULL; name++)
        g_signal_connect(view->text, *name, G_CALLBACK(cursor_changed), view);
    g_signal_connect(view->text, "preedit-changed", G_CALLBACK(preedit_changed), view);
    g_signal_connect(view->widgets.retry, "notify::visible", G_CALLBACK(retry_changed), view);
    g_signal_connect(view->settings, "notify::gtk-enable-animations", G_CALLBACK(settings_changed),
                     view);
    g_signal_connect(view->settings, "notify::gtk-theme-name", G_CALLBACK(settings_changed), view);
    g_signal_connect(view->settings, "notify::gtk-cursor-blink", G_CALLBACK(settings_changed),
                     view);
}
tl_status popup_view_create(GtkWidget *window, tl_popup_view **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (!GTK_IS_WINDOW(window))
        return TL_INVALID;
    tl_popup_view *view = g_try_new0(tl_popup_view, 1);
    if (view == NULL)
        return TL_NOMEM;
    view->window = window;
    build_content(view);
    install_style(view);
    connect_signals(view);
    settings_changed(NULL, NULL, view);
    *out = view;
    return TL_OK;
}
tl_popup_widgets popup_view_widgets(tl_popup_view *view) {
    return view->widgets;
}
void popup_view_set_status(tl_popup_view *view, const char *text) {
    gtk_label_set_text(GTK_LABEL(view->widgets.status), text);
    const char *compact = g_str_has_prefix(text, "No matches.")             ? "No matches."
                          : strcmp(text, "Search service unavailable") == 0 ? "Search offline."
                                                                            : text;
    gtk_label_set_text(GTK_LABEL(view->message), compact);
    gtk_widget_set_tooltip_text(view->message, text);
    update_message(view);
    if (!empty(view))
        gtk_accessible_announce(
            GTK_ACCESSIBLE(view->results == 0 ? view->message : view->widgets.status), text,
            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}
void popup_view_set_results(tl_popup_view *view, size_t count) {
    view->results = count;
    gtk_widget_set_visible(view->widgets.scroll, count != 0);
    gtk_widget_set_visible(view->footer, count != 0);
    if (count != 0)
        gtk_widget_add_css_class(view->widgets.root, "has-results");
    else
        gtk_widget_remove_css_class(view->widgets.root, "has-results");
    update_message(view);
}
static GtkWidget *result_icon(const tl_popup_row *row) {
    GtkWidget *image =
        gtk_image_new_from_icon_name(row->folder ? "folder-symbolic" : "text-x-generic-symbolic");
    if (row->application) {
        GIcon *icon = row->icon[0] == 0 ? NULL : g_icon_new_for_string(row->icon, NULL);
        if (icon != NULL) {
            gtk_image_set_from_gicon(GTK_IMAGE(image), icon);
            g_object_unref(icon);
        } else
            gtk_image_set_from_icon_name(GTK_IMAGE(image), "application-x-executable-symbolic");
    }
    gtk_image_set_pixel_size(GTK_IMAGE(image), 20);
    gtk_widget_add_css_class(image, "result-icon");
    return image;
}
GtkWidget *popup_view_result(const tl_popup_row *row) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_box_append(GTK_BOX(box), result_icon(row));
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(text, true);
    GtkWidget *name = label(row->name, "result-name");
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_size_request(name, -1, 22);
    if (row->application)
        gtk_widget_add_css_class(name, "application-name");
    gtk_box_append(GTK_BOX(text), name);
    char parent[POPUP_PATH_BYTES];
    g_strlcpy(parent, row->display, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash != NULL)
        slash[slash == parent ? 1 : 0] = 0;
    GtkWidget *detail =
        row->application ? label(row->settings ? "System settings" : "Application", "result-detail")
                         : popup_path_label_new(parent);
    gtk_widget_set_size_request(detail, -1, 18);
    gtk_box_append(GTK_BOX(text), detail);
    gtk_box_append(GTK_BOX(box), text);
    GtkWidget *kind = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(kind), label(row->application ? "app"
                                                   : row->folder    ? "folder"
                                                                    : "file",
                                                   "result-kind"));
    GtkWidget *open = label("open ↵", "result-open");
    gtk_widget_add_css_class(open, "result-kind");
    gtk_overlay_add_overlay(GTK_OVERLAY(kind), open);
    gtk_widget_set_size_request(kind, 62, -1);
    gtk_box_append(GTK_BOX(box), kind);
    gtk_widget_set_tooltip_text(box, row->display);
    return box;
}
void popup_view_configure(tl_popup_view *view, int width, int height_limit) {
    if (width < 560)
        gtk_widget_add_css_class(view->widgets.root, "compact");
    else
        gtk_widget_remove_css_class(view->widgets.root, "compact");
    gtk_widget_set_visible(view->reveal, width >= 560);
    bool visible = gtk_widget_get_visible(view->footer);
    gtk_widget_set_visible(view->footer, true);
    gtk_widget_set_visible(view->widgets.scroll, false);
    int minimum, natural;
    gtk_widget_measure(view->widgets.root, GTK_ORIENTATION_VERTICAL, width, &minimum, &natural,
                       NULL, NULL);
    gtk_widget_set_visible(view->footer, visible);
    gtk_widget_set_visible(view->widgets.scroll, view->results != 0);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(view->widgets.scroll),
                                               MIN(8 * 58 + 17, MAX(32, height_limit - natural)));
}
void popup_view_set_active(tl_popup_view *view, bool active) {
    view->active = active;
    remove_timer(&view->typing_timer);
    gtk_widget_remove_css_class(view->surface, "typing");
    reset_caret(view);
}
void popup_view_destroy(tl_popup_view *view) {
    if (view == NULL)
        return;
    remove_timer(&view->typing_timer);
    remove_timer(&view->caret_timer);
    g_signal_handlers_disconnect_by_data(view->settings, view);
    g_signal_handlers_disconnect_by_data(view->widgets.entry, view);
    g_signal_handlers_disconnect_by_data(view->widgets.retry, view);
    g_signal_handlers_disconnect_by_data(view->text, view);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(view->caret), NULL, NULL, NULL);
    gtk_style_context_remove_provider_for_display(view->display, GTK_STYLE_PROVIDER(view->css));
    g_object_unref(view->css);
    g_object_unref(view->display);
    g_object_unref(view->settings);
    g_object_unref(view->widgets.root);
    g_free(view);
}
