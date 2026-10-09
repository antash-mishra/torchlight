/* Theme-following GTK presentation: native search field, gliding selection and bounded motion. */
#include "view.h"
#include "path_label.h"
#include "selection_track.h"
#include <stdio.h>
#include <string.h>
#define TYPING_IDLE_MS 700
/* Longest opening effect (torch sweep: 60ms delay + 560ms) plus a frame of slack. */
#define OPENING_MS 700
/* Matches the popup-close keyframes; the window hides when the fade has finished. */
#define CLOSING_MS 110
#define SPINNER_FADE_MS 120
/* Rows past this share the last cascade delay, so long lists never feel slow. */
#define CASCADE_STEPS 8U
#define ROW_HEIGHT 58
#define VISIBLE_ROWS 8
#define LIST_PADDING 8
#define COMPACT_WIDTH 560
#define RESULT_ICON_SIZE 32
/* Room for "10 results" before optional key hints give way. */
#define STATUS_CHARS 10
#define SEARCH_ICON_SIZE 16
struct popup_view {
    tl_popup_widgets widgets;
    GtkWidget *window, *field, *footer, *message, *message_box, *text, *select, *reveal, *icons,
        *spinner, *track;
    GtkSettings *settings;
    GdkDisplay *display;
    GtkCssProvider *css;
    guint typing_timer, opening_timer, closing_timer;
    size_t results;
    bool active, animations;
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
/* Shown only with something to say, so the field (and the query) never shifts while typing. */
static void update_message(tl_popup_view *view) {
    bool retry = gtk_widget_get_visible(view->widgets.retry);
    bool message = gtk_label_get_text(GTK_LABEL(view->message))[0] != 0;
    gtk_widget_set_visible(view->message_box,
                           !empty(view) && (retry || (message && view->results == 0)));
}
static void retry_changed(GObject *object, GParamSpec *parameter, gpointer context) {
    (void)object;
    (void)parameter;
    update_message(context);
}
static gboolean typing_finished(gpointer context) {
    tl_popup_view *view = context;
    view->typing_timer = 0;
    gtk_widget_remove_css_class(view->field, "typing");
    return G_SOURCE_REMOVE;
}
static void editing_changed(GtkEditable *entry, gpointer context) {
    (void)entry;
    tl_popup_view *view = context;
    remove_timer(&view->typing_timer);
    if (view->active && gtk_widget_has_focus(view->text)) {
        gtk_widget_add_css_class(view->field, "typing");
        view->typing_timer = g_timeout_add(TYPING_IDLE_MS, typing_finished, view);
    }
    update_message(view);
}
static void focus_changed(GObject *object, GParamSpec *parameter, gpointer context) {
    (void)object;
    (void)parameter;
    tl_popup_view *view = context;
    if (gtk_widget_has_focus(view->text))
        return;
    remove_timer(&view->typing_timer);
    gtk_widget_remove_css_class(view->field, "typing");
}
static GtkWidget *label(const char *text, const char *class) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_widget_add_css_class(widget, class);
    return widget;
}
/* A key hint: NULL-terminated key names drawn as keycaps, then the action. */
static GtkWidget *hint(const char *const *keys, const char *action) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    for (; *keys != NULL; keys++)
        gtk_box_append(GTK_BOX(box), label(*keys, "keycap"));
    GtkWidget *text = label(action, "hint-label");
    gtk_widget_set_margin_start(text, 3);
    gtk_box_append(GTK_BOX(box), text);
    return box;
}
static GtkWidget *create_icons(tl_popup_view *view) {
    view->icons = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(view->icons), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(view->icons), SPINNER_FADE_MS);
    GtkWidget *icon = gtk_image_new_from_icon_name("system-search-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), SEARCH_ICON_SIZE);
    gtk_widget_add_css_class(icon, "search-icon");
    view->spinner = gtk_spinner_new();
    gtk_widget_set_name(view->spinner, "popup-spinner");
    gtk_stack_add_named(GTK_STACK(view->icons), icon, "search");
    gtk_stack_add_named(GTK_STACK(view->icons), view->spinner, "spinner");
    gtk_widget_set_valign(view->icons, GTK_ALIGN_CENTER);
    gtk_accessible_update_state(GTK_ACCESSIBLE(view->icons), GTK_ACCESSIBLE_STATE_HIDDEN, true, -1);
    return view->icons;
}
static GtkWidget *create_entry(tl_popup_view *view) {
    view->widgets.entry = gtk_entry_new();
    gtk_entry_set_has_frame(GTK_ENTRY(view->widgets.entry), false);
    const char *name = "Search apps, settings, files and folders";
    gtk_entry_set_placeholder_text(GTK_ENTRY(view->widgets.entry), name);
    gtk_editable_set_width_chars(GTK_EDITABLE(view->widgets.entry), 1);
    gtk_widget_set_hexpand(view->widgets.entry, true);
    gtk_widget_set_valign(view->widgets.entry, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(view->widgets.entry, "query-entry");
    gtk_widget_set_name(view->widgets.entry, "popup-query");
    view->text = GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(view->widgets.entry)));
    gtk_accessible_update_property(GTK_ACCESSIBLE(view->widgets.entry),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
    gtk_accessible_update_property(GTK_ACCESSIBLE(view->text), GTK_ACCESSIBLE_PROPERTY_LABEL, name,
                                   -1);
    return view->widgets.entry;
}
/* No-match, offline and limit feedback sits inside the field, so the popup never grows. */
static GtkWidget *create_message(tl_popup_view *view) {
    view->message_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(view->message_box, "inline-status");
    gtk_widget_set_valign(view->message_box, GTK_ALIGN_CENTER);
    view->message = label("", "message");
    gtk_label_set_ellipsize(GTK_LABEL(view->message), PANGO_ELLIPSIZE_END);
    view->widgets.retry = gtk_button_new_with_label("Retry");
    gtk_widget_set_name(view->widgets.retry, "popup-retry");
    gtk_widget_add_css_class(view->widgets.retry, "retry");
    gtk_widget_set_visible(view->widgets.retry, false);
    gtk_box_append(GTK_BOX(view->message_box), view->message);
    gtk_box_append(GTK_BOX(view->message_box), view->widgets.retry);
    gtk_widget_set_visible(view->message_box, false);
    return view->message_box;
}
static GtkWidget *create_search(tl_popup_view *view) {
    GtkWidget *area = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(area, "search-area");
    view->field = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(view->field, "search-field");
    gtk_widget_set_name(view->field, "popup-search");
    gtk_box_append(GTK_BOX(view->field), create_icons(view));
    gtk_box_append(GTK_BOX(view->field), create_entry(view));
    gtk_box_append(GTK_BOX(view->field), create_message(view));
    gtk_box_append(GTK_BOX(area), view->field);
    return area;
}
static GtkWidget *create_footer(tl_popup_view *view) {
    view->footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_widget_add_css_class(view->footer, "footer");
    gtk_widget_set_name(view->footer, "popup-footer");
    view->widgets.status = label("", "status");
    gtk_widget_set_name(view->widgets.status, "popup-status");
    gtk_label_set_ellipsize(GTK_LABEL(view->widgets.status), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(view->widgets.status), STATUS_CHARS);
    gtk_widget_set_hexpand(view->widgets.status, true);
    gtk_box_append(GTK_BOX(view->footer), view->widgets.status);
    view->select = hint((const char *const[]){"↑", "↓", NULL}, "Select");
    gtk_box_append(GTK_BOX(view->footer), view->select);
    gtk_box_append(GTK_BOX(view->footer), hint((const char *const[]){"Enter", NULL}, "Open"));
    view->reveal = hint((const char *const[]){"Ctrl", "Enter", NULL}, "Show in Folder");
    gtk_box_append(GTK_BOX(view->footer), view->reveal);
    gtk_box_append(GTK_BOX(view->footer), hint((const char *const[]){"Esc", NULL}, "Close"));
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
}
static GtkWidget *create_results(tl_popup_view *view) {
    view->widgets.list = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(view->widgets.list), true);
    gtk_widget_set_focusable(view->widgets.list, false);
    view->track = popup_selection_track_new(view->widgets.list);
    view->widgets.scroll = gtk_scrolled_window_new();
    gtk_widget_set_name(view->widgets.scroll, "popup-results");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(view->widgets.scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(view->widgets.scroll),
                                                     true);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(view->widgets.scroll),
                                               VISIBLE_ROWS * ROW_HEIGHT + LIST_PADDING);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(view->widgets.scroll), view->track);
    gtk_widget_set_visible(view->widgets.scroll, false);
    return view->widgets.scroll;
}
static void build_content(tl_popup_view *view) {
    view->widgets.root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(view->widgets.root);
    gtk_widget_add_css_class(view->widgets.root, "launcher-surface");
    gtk_widget_set_overflow(view->widgets.root, GTK_OVERFLOW_HIDDEN);
    view->settings = g_object_ref(gtk_widget_get_settings(view->window));
    gtk_box_append(GTK_BOX(view->widgets.root), create_search(view));
    gtk_box_append(GTK_BOX(view->widgets.root), create_results(view));
    gtk_box_append(GTK_BOX(view->widgets.root), create_footer(view));
    gtk_window_set_child(GTK_WINDOW(view->window), view->widgets.root);
}
static void install_style(tl_popup_view *view) {
    view->display = g_object_ref(gtk_widget_get_display(view->window));
    view->css = gtk_css_provider_new();
    gtk_css_provider_load_from_resource(view->css, "/org/torchlight/launcher/popup.css");
    gtk_style_context_add_provider_for_display(view->display, GTK_STYLE_PROVIDER(view->css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}
static void connect_signals(tl_popup_view *view) {
    g_signal_connect(view->widgets.entry, "changed", G_CALLBACK(editing_changed), view);
    g_signal_connect(view->text, "notify::has-focus", G_CALLBACK(focus_changed), view);
    g_signal_connect(view->widgets.retry, "notify::visible", G_CALLBACK(retry_changed), view);
    g_signal_connect(view->settings, "notify::gtk-enable-animations", G_CALLBACK(settings_changed),
                     view);
    g_signal_connect(view->settings, "notify::gtk-theme-name", G_CALLBACK(settings_changed), view);
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
    /* The field's spinner already shows a pending search, so it needs no inline text. */
    const char *compact = g_str_has_prefix(text, "No matches.")             ? "No matches"
                          : strcmp(text, "Search service unavailable") == 0 ? "Search offline"
                          : strcmp(text, "Searching…") == 0                 ? ""
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
static GIcon *file_icon(const tl_popup_row *row) {
    if (row->folder)
        return g_themed_icon_new("folder");
    gboolean uncertain = false;
    /* Guess from the name alone, without reading the file. Names the guess cannot
     * place, such as extensionless headers, keep a generic document icon. */
    char *type = g_content_type_guess(row->name, NULL, 0, &uncertain);
    GIcon *icon = uncertain ? g_themed_icon_new("text-x-generic") : g_content_type_get_icon(type);
    g_free(type);
    return icon;
}
static GtkWidget *result_icon(const tl_popup_row *row) {
    GIcon *icon = NULL;
    if (row->application && row->icon[0] != 0)
        icon = g_icon_new_for_string(row->icon, NULL);
    if (icon == NULL)
        icon = row->application ? g_themed_icon_new("application-x-executable") : file_icon(row);
    GtkWidget *image = gtk_image_new_from_gicon(icon);
    g_object_unref(icon);
    gtk_image_set_pixel_size(GTK_IMAGE(image), RESULT_ICON_SIZE);
    gtk_widget_add_css_class(image, "result-icon");
    return image;
}
static GtkWidget *result_detail(const tl_popup_row *row, size_t path_keep) {
    if (row->application)
        return label(row->settings ? "System settings" : "Application", "result-detail");
    char parent[POPUP_PATH_BYTES];
    g_strlcpy(parent, row->display, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash != NULL)
        slash[slash == parent ? 1 : 0] = 0;
    return popup_path_label_new(parent, path_keep);
}
static void add_entrance(GtkWidget *result, tl_popup_entrance entrance, size_t index) {
    if (entrance == POPUP_ENTER_FADE)
        gtk_widget_add_css_class(result, "fresh");
    if (entrance != POPUP_ENTER_CASCADE)
        return;
    char class[16];
    int written = snprintf(class, sizeof(class), "enter-%zu", MIN(index, CASCADE_STEPS - 1));
    if (written > 0 && (size_t)written < sizeof(class))
        gtk_widget_add_css_class(result, class);
}
GtkWidget *popup_view_result(const tl_popup_row *row, size_t path_keep, tl_popup_entrance entrance,
                             size_t index) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(box, "result");
    add_entrance(box, entrance, index);
    gtk_box_append(GTK_BOX(box), result_icon(row));
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(text, true);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    GtkWidget *name = label(row->name, "result-name");
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(text), name);
    gtk_box_append(GTK_BOX(text), result_detail(row, path_keep));
    gtk_box_append(GTK_BOX(box), text);
    gtk_widget_set_tooltip_text(box, row->display);
    return box;
}
void popup_view_select(tl_popup_view *view, GtkWidget *row, bool glide) {
    popup_selection_track_set_target(view->track, row, glide);
}
void popup_view_set_searching(tl_popup_view *view, bool searching) {
    gtk_stack_set_visible_child_name(GTK_STACK(view->icons), searching ? "spinner" : "search");
    gtk_spinner_set_spinning(GTK_SPINNER(view->spinner), searching);
}
void popup_view_set_launching(tl_popup_view *view, bool launching) {
    GtkWidget *highlight = popup_selection_track_get_highlight(view->track);
    GtkWidget *row = popup_selection_track_get_target(view->track);
    GtkWidget *result = row != NULL && GTK_IS_LIST_BOX_ROW(row)
                            ? gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row))
                            : NULL;
    if (launching) {
        gtk_widget_add_css_class(highlight, "flash");
        if (result != NULL)
            gtk_widget_add_css_class(result, "launching");
        return;
    }
    gtk_widget_remove_css_class(highlight, "flash");
    if (result != NULL)
        gtk_widget_remove_css_class(result, "launching");
}
/* Large desktop fonts must never widen the popup: drop the least essential hints
 * (Select, then Show in Folder) until the footer fits. Open and Close stay. */
static void fit_hints(tl_popup_view *view, int width) {
    GtkWidget *optional[] = {view->select, view->reveal};
    gtk_widget_set_visible(view->select, true);
    gtk_widget_set_visible(view->reveal, width >= COMPACT_WIDTH);
    for (size_t i = 0; i < G_N_ELEMENTS(optional); i++) {
        int minimum, natural;
        /* Measure the whole surface so its border and padding are counted too. */
        gtk_widget_measure(view->widgets.root, GTK_ORIENTATION_HORIZONTAL, -1, &minimum, &natural,
                           NULL, NULL);
        if (minimum <= width)
            return;
        gtk_widget_set_visible(optional[i], false);
    }
}
void popup_view_configure(tl_popup_view *view, int width, int height_limit) {
    if (width < COMPACT_WIDTH)
        gtk_widget_add_css_class(view->widgets.root, "compact");
    else
        gtk_widget_remove_css_class(view->widgets.root, "compact");
    bool visible = gtk_widget_get_visible(view->footer);
    gtk_widget_set_visible(view->footer, true);
    fit_hints(view, width);
    gtk_widget_set_visible(view->widgets.scroll, false);
    int minimum, natural;
    gtk_widget_measure(view->widgets.root, GTK_ORIENTATION_VERTICAL, width, &minimum, &natural,
                       NULL, NULL);
    gtk_widget_set_visible(view->footer, visible);
    gtk_widget_set_visible(view->widgets.scroll, view->results != 0);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(view->widgets.scroll),
        MIN(VISIBLE_ROWS * ROW_HEIGHT + LIST_PADDING, MAX(32, height_limit - natural)));
}
static gboolean opening_finished(gpointer context) {
    tl_popup_view *view = context;
    view->opening_timer = 0;
    /* Removing the class lets the next show restart the opening animations. */
    gtk_widget_remove_css_class(view->widgets.root, "opening");
    return G_SOURCE_REMOVE;
}
void popup_view_set_active(tl_popup_view *view, bool active) {
    view->active = active;
    remove_timer(&view->typing_timer);
    remove_timer(&view->opening_timer);
    remove_timer(&view->closing_timer);
    gtk_widget_remove_css_class(view->field, "typing");
    gtk_widget_remove_css_class(view->widgets.root, "opening");
    gtk_widget_remove_css_class(view->widgets.root, "closing");
    if (!active)
        return;
    popup_view_set_launching(view, false);
    popup_view_set_searching(view, false);
    if (!view->animations)
        return;
    gtk_widget_add_css_class(view->widgets.root, "opening");
    view->opening_timer = g_timeout_add(OPENING_MS, opening_finished, view);
}
static gboolean closing_finished(gpointer context) {
    tl_popup_view *view = context;
    view->closing_timer = 0;
    /* Hide before restoring opacity so no full-opacity frame can be drawn. */
    gtk_widget_set_visible(view->window, false);
    gtk_widget_remove_css_class(view->widgets.root, "closing");
    return G_SOURCE_REMOVE;
}
void popup_view_dismiss(tl_popup_view *view) {
    bool fade = view->active && view->animations && gtk_widget_get_mapped(view->window);
    popup_view_set_active(view, false);
    popup_view_set_searching(view, false);
    if (!fade) {
        gtk_widget_set_visible(view->window, false);
        return;
    }
    gtk_widget_add_css_class(view->widgets.root, "closing");
    view->closing_timer = g_timeout_add(CLOSING_MS, closing_finished, view);
}
void popup_view_destroy(tl_popup_view *view) {
    if (view == NULL)
        return;
    remove_timer(&view->typing_timer);
    remove_timer(&view->opening_timer);
    remove_timer(&view->closing_timer);
    g_signal_handlers_disconnect_by_data(view->settings, view);
    g_signal_handlers_disconnect_by_data(view->widgets.entry, view);
    g_signal_handlers_disconnect_by_data(view->widgets.retry, view);
    g_signal_handlers_disconnect_by_data(view->text, view);
    gtk_style_context_remove_provider_for_display(view->display, GTK_STYLE_PROVIDER(view->css));
    g_object_unref(view->css);
    g_object_unref(view->display);
    g_object_unref(view->settings);
    g_object_unref(view->widgets.root);
    g_free(view);
}
