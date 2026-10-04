/* Native GTK4 single-instance launcher; all IPC and launch I/O are asynchronous. */
#include "torchlight/launcher.h"
#include "actions.h"
#include "torchlight/async.h"
#include "torchlight/popup.h"
#include <gio/gdesktopappinfo.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <X11/Xlib.h>
#include <gdk/x11/gdkx.h>
#endif
#include <stdio.h>
#include <string.h>
#define POPUP_DEBOUNCE_MS 16
#define POPUP_PENDING_MS 120
#define POPUP_STATUS_MS 2000
struct popup {
    GtkApplication *application;
    GtkWidget *window, *entry, *list, *status, *retry, *scroll;
    tl_popup_model *model;
    char *socket_path;
    uint64_t sequence;
    char request_id[IPC_REQUEST_ID_BYTES + 1];
    guint debounce, pending, status_timer;
    tl_ipc_exchange *query, *action, *status_exchange, *history;
    bool dirty, launching, visible;
    GCancellable *launch_cancel;
};
static void start_query(struct popup *popup);
static void set_status(struct popup *popup, const char *text) {
    gtk_label_set_text(GTK_LABEL(popup->status), text);
    gtk_accessible_announce(GTK_ACCESSIBLE(popup->status), text,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
}
static void remove_timer(guint *timer) {
    if (*timer != 0)
        g_source_remove(*timer);
    *timer = 0;
}
static bool query_empty(struct popup *popup) {
    const char *text = gtk_editable_get_text(GTK_EDITABLE(popup->entry));
    for (; *text != 0; text = g_utf8_next_char(text))
        if (!g_unichar_isspace(g_utf8_get_char(text)))
            return false;
    return true;
}
static void close_popup(struct popup *popup) {
    popup->visible = false;
    popup->dirty = false;
    remove_timer(&popup->debounce);
    remove_timer(&popup->pending);
    ipc_exchange_destroy(popup->query);
    popup->query = NULL;
    ipc_exchange_destroy(popup->action);
    popup->action = NULL;
    if (popup->launch_cancel != NULL)
        g_cancellable_cancel(popup->launch_cancel);
    gtk_widget_set_visible(popup->window, false);
}
static void select_row(struct popup *popup) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(popup->list),
                                                       (int)popup_model_position(popup->model));
    gtk_list_box_select_row(GTK_LIST_BOX(popup->list), row);
    const tl_popup_row *selected = popup_model_selected(popup->model);
    if (selected != NULL)
        gtk_accessible_announce(GTK_ACCESSIBLE(popup->list), selected->name,
                                GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    if (row == NULL)
        return;
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(GTK_WIDGET(row), popup->list, &bounds))
        return;
    GtkAdjustment *adjustment =
        gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(popup->scroll));
    double top = (double)bounds.origin.y, bottom = top + (double)bounds.size.height;
    double value = gtk_adjustment_get_value(adjustment);
    double page = gtk_adjustment_get_page_size(adjustment);
    if (top < value)
        gtk_adjustment_set_value(adjustment, top);
    else if (bottom > value + page)
        gtk_adjustment_set_value(adjustment, bottom - page);
}
static GtkWidget *result_widget(const tl_popup_row *row) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
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
    gtk_image_set_pixel_size(GTK_IMAGE(image), 24);
    gtk_box_append(GTK_BOX(box), image);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, true);
    GtkWidget *name = gtk_label_new(row->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(name, "result-name");
    gtk_box_append(GTK_BOX(text), name);
    char parent[POPUP_PATH_BYTES];
    g_strlcpy(parent, row->display, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent)
        *slash = 0;
    const char *subtitle = row->application ? (row->settings ? "Settings" : "Application") : parent;
    GtkWidget *detail = gtk_label_new(subtitle);
    gtk_label_set_xalign(GTK_LABEL(detail), 0);
    gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(detail, "dim-label");
    gtk_widget_add_css_class(detail, "result-detail");
    gtk_box_append(GTK_BOX(text), detail);
    gtk_box_append(GTK_BOX(box), text);
    gtk_widget_set_size_request(box, -1, 58);
    gtk_widget_set_tooltip_text(box, row->display);
    return box;
}
static void render(struct popup *popup) {
    gtk_list_box_remove_all(GTK_LIST_BOX(popup->list));
    size_t count = popup_model_count(popup->model);
    for (size_t i = 0; i < count; i++)
        gtk_list_box_append(GTK_LIST_BOX(popup->list),
                            result_widget(popup_model_row(popup->model, i)));
    gtk_widget_set_sensitive(popup->list, true);
    select_row(popup);
    if (query_empty(popup))
        set_status(popup, "Type to search");
    else if (count == 0)
        set_status(popup, "No matches. Try a name or part of its folder path.");
    else if (popup_model_degraded(popup->model))
        set_status(popup, "Some folders are unavailable");
    else if (popup_model_indexing(popup->model))
        set_status(popup, "Updating index…");
    else {
        char message[64];
        int written =
            snprintf(message, sizeof(message), "%zu result%s", count, count == 1 ? "" : "s");
        if (written > 0 && (size_t)written < sizeof(message))
            set_status(popup, message);
    }
    gtk_widget_set_visible(popup->retry, false);
}
static gboolean pending_status(gpointer context) {
    struct popup *popup = context;
    popup->pending = 0;
    if (popup->visible && (popup->query != NULL || popup->dirty))
        set_status(popup, "Searching…");
    return G_SOURCE_REMOVE;
}
static void query_response(void *context, tl_status status, const char *response, size_t length,
                           bool terminal) {
    struct popup *popup = context;
    if (status == TL_OK)
        status = popup_model_apply(popup->model, response, length);
    if (status == TL_OK && popup->visible)
        render(popup);
    else if (status != TL_STATE && popup->visible && !popup->dirty) {
        set_status(popup, "Search service unavailable");
        gtk_widget_set_visible(popup->retry, true);
    }
    if (!terminal)
        return;
    ipc_exchange_destroy(popup->query);
    popup->query = NULL;
    remove_timer(&popup->pending);
    if (popup->dirty && popup->visible)
        start_query(popup);
}
static void start_query(struct popup *popup) {
    if (!popup->visible || popup->query != NULL || !popup->dirty)
        return;
    if (query_empty(popup)) {
        popup->dirty = false;
        return;
    }
    const char *text = gtk_editable_get_text(GTK_EDITABLE(popup->entry));
    if (strlen(text) > LEXICAL_QUERY_BYTES) {
        popup->dirty = false;
        set_status(popup, "Search is limited to 256 bytes. Shorten the pasted text.");
        return;
    }
    tl_ipc_request request = {.operation = IPC_QUERY, .limit = POPUP_RESULTS};
    g_strlcpy(request.request_id, popup->request_id, sizeof(request.request_id));
    g_strlcpy(request.query, text, sizeof(request.query));
    popup->dirty = false;
    tl_status status =
        ipc_exchange_create(popup->socket_path, &request, query_response, popup, &popup->query);
    if (status != TL_OK) {
        set_status(popup, "Search service unavailable");
        gtk_widget_set_visible(popup->retry, true);
    }
    remove_timer(&popup->pending);
    popup->pending = g_timeout_add(POPUP_PENDING_MS, pending_status, popup);
}
static gboolean debounce(gpointer context) {
    struct popup *popup = context;
    popup->debounce = 0;
    start_query(popup);
    return G_SOURCE_REMOVE;
}
static void changed(GtkEditable *entry, gpointer context) {
    (void)entry;
    struct popup *popup = context;
    popup->sequence++;
    int written = snprintf(popup->request_id, sizeof(popup->request_id), "query-%llu",
                           (unsigned long long)popup->sequence);
    tl_status status = written > 0 && (size_t)written < sizeof(popup->request_id)
                           ? popup_model_begin(popup->model, popup->request_id)
                           : TL_LIMIT;
    if (status != TL_OK) {
        set_status(popup, "Could not start search");
        return;
    }
    ipc_exchange_destroy(popup->action);
    popup->action = NULL;
    if (popup->launch_cancel != NULL)
        g_cancellable_cancel(popup->launch_cancel);
    popup->dirty = true;
    gtk_widget_set_sensitive(popup->list, false);
    remove_timer(&popup->debounce);
    if (query_empty(popup)) {
        popup->dirty = false;
        remove_timer(&popup->pending);
        ipc_exchange_destroy(popup->query);
        popup->query = NULL;
        popup_model_clear(popup->model);
        render(popup);
        return;
    }
    popup->debounce = g_timeout_add(POPUP_DEBOUNCE_MS, debounce, popup);
}
static void launch_free(gpointer context) {
    struct launch *launch = context;
    if (launch->context != NULL)
        g_object_unref(launch->context);
    launch->context = NULL;
    g_free(launch);
}
static void history_response(void *context, tl_status status, const char *response, size_t length,
                             bool terminal) {
    (void)status;
    (void)response;
    (void)length;
    struct popup *popup = context;
    if (terminal) {
        ipc_exchange_destroy(popup->history);
        popup->history = NULL;
    }
}
static void launched(GObject *source, GAsyncResult *result, gpointer context) {
    (void)source;
    struct popup *popup = context;
    GTask *task = G_TASK(result);
    struct launch *launch = g_task_get_task_data(task);
    GError *error = NULL;
    bool accepted = g_task_propagate_boolean(task, &error);
    bool canceled = g_cancellable_is_cancelled(g_task_get_cancellable(task));
    g_clear_error(&error);
    popup->launching = false;
    if (popup->launch_cancel != NULL)
        g_object_unref(popup->launch_cancel);
    popup->launch_cancel = NULL;
    if (!accepted) {
        if (popup->visible && !canceled)
            set_status(popup,
                       launch->reveal ? "Could not reveal this item" : "Could not open this item");
        return;
    }
    ipc_exchange_destroy(popup->history);
    popup->history = NULL;
    tl_status status = ipc_exchange_create(popup->socket_path, &launch->event, history_response,
                                           popup, &popup->history);
    if (status != TL_OK && !canceled)
        set_status(popup, "Opened; history could not be recorded");
    /* An accepted action still earns history after cancellation, but its
     * completion must not dismiss a popup reopened for a newer search. */
    if (!canceled)
        close_popup(popup);
}
static void resolved(void *context, tl_status status, const char *response, size_t length,
                     bool terminal) {
    struct popup *popup = context;
    if (!terminal)
        return;
    struct launch *launch = g_object_get_data(G_OBJECT(popup->window), "launch");
    size_t count = 0;
    char search[IPC_HISTORY_ID_BYTES + 1];
    if (status == TL_OK)
        status =
            popup_model_decode(popup->model, response, length, &launch->row, 1, &count, search);
    ipc_exchange_destroy(popup->action);
    popup->action = NULL;
    if (status != TL_OK || count != 1) {
        set_status(popup, status == TL_STATE ? "This item is no longer indexed"
                                             : "Could not resolve this item");
        if (status == TL_STATE) {
            popup->dirty = true;
            start_query(popup);
        }
        return;
    }
    GdkAppLaunchContext *launch_context =
        gdk_display_get_app_launch_context(gtk_widget_get_display(popup->window));
    launch->context = G_APP_LAUNCH_CONTEXT(launch_context);
    popup->launching = true;
    popup->launch_cancel = g_cancellable_new();
    GTask *task = g_task_new(popup->application, popup->launch_cancel, launched, popup);
    g_object_steal_data(G_OBJECT(popup->window), "launch");
    g_task_set_task_data(task, launch, launch_free);
    g_task_run_in_thread(task, actions_worker);
    g_object_unref(task);
}
static void activate_selected(struct popup *popup, bool reveal) {
    const tl_popup_row *row = popup_model_selected(popup->model);
    if (row == NULL || popup->action != NULL || popup->launching)
        return;
    struct launch *launch = g_try_new0(struct launch, 1);
    if (launch == NULL) {
        set_status(popup, "Could not prepare this action");
        return;
    }
    launch->row = *row;
    launch->reveal = reveal;
    launch->event.operation = IPC_OPEN;
    launch->event.file_id = row->id;
    char *uuid = g_uuid_string_random();
    g_strlcpy(launch->event.event_id, uuid, sizeof(launch->event.event_id));
    g_free(uuid);
    g_strlcpy(launch->event.search_id, popup_model_search_id(popup->model),
              sizeof(launch->event.search_id));
    g_strlcpy(launch->event.request_id, launch->event.event_id, sizeof(launch->event.request_id));
    tl_ipc_request request = {.operation = IPC_RESOLVE, .file_id = row->id};
    g_strlcpy(request.request_id, launch->event.request_id, sizeof(request.request_id));
    g_object_set_data_full(G_OBJECT(popup->window), "launch", launch, launch_free);
    tl_status status =
        ipc_exchange_create(popup->socket_path, &request, resolved, popup, &popup->action);
    if (status != TL_OK)
        set_status(popup, "Could not resolve this item");
}
static gboolean key_pressed(GtkEventControllerKey *controller, guint key, guint keycode,
                            GdkModifierType modifiers, gpointer context) {
    (void)controller;
    (void)keycode;
    struct popup *popup = context;
    if (key == GDK_KEY_Escape) {
        close_popup(popup);
        return true;
    }
    if (gtk_widget_has_focus(popup->retry))
        return false;
    if (key == GDK_KEY_Up || key == GDK_KEY_Down) {
        popup_model_move(popup->model, key == GDK_KEY_Up ? -1 : 1);
        select_row(popup);
        return true;
    }
    if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
        activate_selected(popup, (modifiers & GDK_CONTROL_MASK) != 0);
        return true;
    }
    return false;
}
static void row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer context) {
    (void)list;
    struct popup *popup = context;
    int position = gtk_list_box_row_get_index(row);
    size_t current = popup_model_position(popup->model);
    if (position < 0 || popup_model_selected(popup->model) == NULL)
        return;
    while (current != (size_t)position) {
        popup_model_move(popup->model, current < (size_t)position ? 1 : -1);
        current = popup_model_position(popup->model);
    }
    activate_selected(popup, false);
}
static gboolean close_requested(GtkWindow *window, gpointer context) {
    (void)window;
    close_popup(context);
    return true;
}
static void active_changed(GObject *window, GParamSpec *parameter, gpointer context) {
    (void)parameter;
    struct popup *popup = context;
    if (popup->visible && !gtk_window_is_active(GTK_WINDOW(window)))
        close_popup(popup);
}
static void retry_clicked(GtkButton *button, gpointer context) {
    (void)button;
    struct popup *popup = context;
    popup->dirty = true;
    start_query(popup);
    gtk_widget_grab_focus(popup->entry);
}
static void status_response(void *context, tl_status status, const char *response, size_t length,
                            bool terminal) {
    struct popup *popup = context;
    if (status == TL_OK && popup->visible && response != NULL) {
        tl_json_token tokens[256];
        tl_json json;
        tl_status decoded = json_parse(response, length, tokens, 256, &json);
        if (decoded == TL_OK) {
            size_t indexing = json_member(&json, 0, "indexing");
            size_t degraded = json_member(&json, indexing, "degraded");
            size_t watching = json_member(&json, indexing, "watch_degraded");
            size_t active = json_member(&json, indexing, "active");
            bool limited = (degraded != SIZE_MAX && json.text[tokens[degraded].start] == 't') ||
                           (watching != SIZE_MAX && json.text[tokens[watching].start] == 't');
            if (limited)
                set_status(popup, "Some folders are unavailable");
            else if (active != SIZE_MAX && json.text[tokens[active].start] == 't')
                set_status(popup, "Updating index…");
            if (gtk_widget_get_visible(popup->retry) && popup->query == NULL) {
                popup->dirty = true;
                start_query(popup);
            }
        }
    }
    if (status != TL_OK && popup->visible && popup_model_selected(popup->model) == NULL) {
        set_status(popup, "Search service unavailable");
        gtk_widget_set_visible(popup->retry, true);
    }
    if (terminal) {
        ipc_exchange_destroy(popup->status_exchange);
        popup->status_exchange = NULL;
    }
}
static gboolean poll_status(gpointer context) {
    struct popup *popup = context;
    if (!popup->visible || popup->status_exchange != NULL)
        return G_SOURCE_CONTINUE;
    tl_ipc_request request = {.operation = IPC_STATUS};
    g_strlcpy(request.request_id, "status", sizeof(request.request_id));
    tl_status status = ipc_exchange_create(popup->socket_path, &request, status_response, popup,
                                           &popup->status_exchange);
    if (status != TL_OK && popup_model_selected(popup->model) == NULL)
        set_status(popup, "Search service unavailable");
    return G_SOURCE_CONTINUE;
}
static void place_window(GtkWidget *widget, gpointer context) {
    struct popup *popup = context;
    (void)widget;
#ifdef GDK_WINDOWING_X11
    GdkDisplay *display = gtk_widget_get_display(popup->window);
    if (!GDK_IS_X11_DISPLAY(display))
        return;
    Display *xdisplay = gdk_x11_display_get_xdisplay(display);
    Window root = DefaultRootWindow(xdisplay), child, root_return;
    int pointer_x = 0, pointer_y = 0, local_x, local_y;
    unsigned mask;
    if (!XQueryPointer(xdisplay, root, &root_return, &child, &pointer_x, &pointer_y, &local_x,
                       &local_y, &mask))
        return;
    int scale = gtk_widget_get_scale_factor(popup->window);
    pointer_x /= scale;
    pointer_y /= scale;
    GListModel *monitors = gdk_display_get_monitors(display);
    for (guint i = 0; i < g_list_model_get_n_items(monitors); i++) {
        GdkMonitor *monitor = g_list_model_get_item(monitors, i);
        GdkRectangle geometry, area;
        gdk_monitor_get_geometry(monitor, &geometry);
        gdk_x11_monitor_get_workarea(monitor, &area);
        g_object_unref(monitor);
        /* Panels belong to their monitor even when outside its usable area. */
        if (pointer_x < geometry.x || pointer_x >= geometry.x + geometry.width ||
            pointer_y < geometry.y || pointer_y >= geometry.y + geometry.height)
            continue;
        int width = MIN(680, MAX(240, area.width - 48));
        gtk_window_set_default_size(GTK_WINDOW(popup->window), width, -1);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(popup->scroll), MIN(8 * 58, MAX(58, area.height * 7 / 10 - 150)));
        GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(popup->window));
        if (surface == NULL)
            break;
        XMoveWindow(xdisplay, gdk_x11_surface_get_xid(surface),
                    (area.x + (area.width - width) / 2) * scale,
                    (area.y + area.height / 5) * scale);
        XFlush(xdisplay);
        break;
    }
#else
    (void)popup;
#endif
}
static GtkWidget *create_footer(struct popup *popup) {
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(footer, "footer");
    gtk_widget_set_size_request(footer, -1, 32);
    popup->status = gtk_label_new("Type to search");
    gtk_label_set_xalign(GTK_LABEL(popup->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(popup->status), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(popup->status, true);
    gtk_box_append(GTK_BOX(footer), popup->status);
    popup->retry = gtk_button_new_with_label("Retry");
    gtk_widget_set_visible(popup->retry, false);
    gtk_box_append(GTK_BOX(footer), popup->retry);
    GtkWidget *hints = gtk_label_new("↑↓ Select  Enter Open  Ctrl+Enter Reveal  Esc Close");
    gtk_widget_add_css_class(hints, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(hints), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(footer), hints);
    return footer;
}
static GtkWidget *create_search_entry(void) {
    const char *label = "Search apps, settings, files and folders";
    GtkWidget *entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(entry), label);
    gtk_widget_set_size_request(entry, -1, 52);
    gtk_accessible_update_property(GTK_ACCESSIBLE(entry), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
    /* GTK exposes the internal editable text separately to AT-SPI. Give it
     * the same name so screen readers can identify the actual input control. */
    GtkEditable *editable = gtk_editable_get_delegate(GTK_EDITABLE(entry));
    if (GTK_IS_ACCESSIBLE(editable))
        gtk_accessible_update_property(GTK_ACCESSIBLE(editable), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       label, -1);
    return entry;
}
static void build_window(struct popup *popup) {
    popup->window = gtk_application_window_new(popup->application);
    gtk_window_set_title(GTK_WINDOW(popup->window), "Torchlight");
    gtk_window_set_decorated(GTK_WINDOW(popup->window), false);
    gtk_window_set_resizable(GTK_WINDOW(popup->window), false);
    gtk_window_set_default_size(GTK_WINDOW(popup->window), 680, -1);
    gtk_widget_add_css_class(popup->window, "torchlight");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    gtk_window_set_child(GTK_WINDOW(popup->window), box);
    popup->entry = create_search_entry();
    gtk_box_append(GTK_BOX(box), popup->entry);
    popup->list = gtk_list_box_new();
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(popup->list), true);
    gtk_widget_set_focusable(popup->list, false);
    popup->scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(popup->scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(popup->scroll), true);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(popup->scroll), 8 * 58);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(popup->scroll), popup->list);
    gtk_box_append(GTK_BOX(box), popup->scroll);
    gtk_box_append(GTK_BOX(box), create_footer(popup));
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), popup);
    gtk_widget_add_controller(popup->window, keys);
    g_signal_connect(popup->entry, "changed", G_CALLBACK(changed), popup);
    g_signal_connect(popup->list, "row-activated", G_CALLBACK(row_activated), popup);
    g_signal_connect(popup->retry, "clicked", G_CALLBACK(retry_clicked), popup);
    g_signal_connect(popup->window, "close-request", G_CALLBACK(close_requested), popup);
    g_signal_connect(popup->window, "notify::is-active", G_CALLBACK(active_changed), popup);
    g_signal_connect(popup->window, "map", G_CALLBACK(place_window), popup);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        css, ".torchlight { border-radius: 12px; } searchentry { font-size: 20px; } "
             ".footer { font-size: 12px; } .result-name { font-size: 15px; } .result-detail { "
             "font-size: 12px; } "
             "row { padding: 0 8px; border-radius: 6px; } row:selected .dim-label { opacity: 1; }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(popup->window),
                                               GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    popup->status_timer = g_timeout_add(POPUP_STATUS_MS, poll_status, popup);
}
static int command_line(GApplication *application, GApplicationCommandLine *command,
                        gpointer context) {
    (void)application;
    struct popup *popup = context;
    GVariantDict *options = g_application_command_line_get_options_dict(command);
    char *path = NULL;
    if (g_variant_dict_lookup(options, "socket", "s", &path)) {
        if (popup->window == NULL) {
            ipc_path_destroy(popup->socket_path);
            popup->socket_path = path;
        } else
            g_free(path);
    }
    if (popup->socket_path == NULL) {
        tl_status status = ipc_default_path(&popup->socket_path);
        if (status != TL_OK) {
            g_application_command_line_printerr(command, "No safe XDG_RUNTIME_DIR socket path\n");
            return 1;
        }
    }
    if (popup->window == NULL)
        build_window(popup);
    else if (popup->visible && gtk_window_is_active(GTK_WINDOW(popup->window))) {
        close_popup(popup);
        return 0;
    }
    popup->visible = true;
    gtk_editable_set_text(GTK_EDITABLE(popup->entry), "");
    changed(GTK_EDITABLE(popup->entry), popup);
    /* Clamp before mapping: window managers otherwise constrain an initially
     * oversized scaled window before GTK applies its smaller default size. */
    gtk_widget_realize(popup->window);
    place_window(popup->window, popup);
    gtk_window_present(GTK_WINDOW(popup->window));
    gtk_widget_grab_focus(popup->entry);
    return 0;
}
tl_status launcher_create(tl_launcher **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    struct popup *popup = g_try_new0(struct popup, 1);
    if (popup == NULL)
        return TL_NOMEM;
    tl_status status = popup_model_create(&popup->model);
    if (status != TL_OK) {
        g_free(popup);
        return status;
    }
    popup->application =
        gtk_application_new("org.torchlight.Launcher", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_application_add_main_option(G_APPLICATION(popup->application), "toggle", 0,
                                  G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
                                  "Show or hide the launcher", NULL);
    g_application_add_main_option(G_APPLICATION(popup->application), "socket", 0,
                                  G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, "Override daemon socket",
                                  "PATH");
    g_signal_connect(popup->application, "command-line", G_CALLBACK(command_line), popup);
    *out = popup;
    return TL_OK;
}
tl_status launcher_run(tl_launcher *launcher, int argc, char **argv, int *exit_code) {
    if (launcher == NULL || argv == NULL || exit_code == NULL)
        return TL_INVALID;
    *exit_code = g_application_run(G_APPLICATION(launcher->application), argc, argv);
    return TL_OK;
}
void launcher_destroy(tl_launcher *popup) {
    if (popup == NULL)
        return;
    remove_timer(&popup->debounce);
    remove_timer(&popup->pending);
    remove_timer(&popup->status_timer);
    ipc_exchange_destroy(popup->query);
    ipc_exchange_destroy(popup->action);
    ipc_exchange_destroy(popup->history);
    ipc_exchange_destroy(popup->status_exchange);
    if (popup->launch_cancel != NULL)
        g_cancellable_cancel(popup->launch_cancel);
    /* GTask keeps its application source alive; finish callbacks before context free. */
    while (popup->launching)
        g_main_context_iteration(NULL, true);
    if (popup->launch_cancel != NULL)
        g_object_unref(popup->launch_cancel);
    popup->launch_cancel = NULL;
    g_object_unref(popup->application);
    ipc_path_destroy(popup->socket_path);
    popup_model_destroy(popup->model);
    g_free(popup);
}
