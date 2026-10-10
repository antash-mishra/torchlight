/* Native GTK4 single-instance launcher; all IPC and launch I/O are asynchronous. */
#include "torchlight/launcher.h"
#include "actions.h"
#include "torchlight/async.h"
#include "torchlight/popup.h"
#include "view.h"
#include "windows_x11.h"
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
/* A list item's identity across re-renders: its result, kind and window. */
struct shown_item {
    uint64_t id, handle;
    tl_popup_item_kind kind;
};
struct popup {
    GtkApplication *application;
    GtkWidget *window, *entry, *list, *retry, *scroll;
    tl_popup_model *model;
    tl_popup_view *view;
    /* Open windows, read once per show; NULL when unavailable. */
    tl_windows *windows;
    char *socket_path;
    uint64_t sequence;
    char request_id[IPC_REQUEST_ID_BYTES + 1];
    guint debounce, pending, status_timer, windows_idle;
    tl_ipc_exchange *query, *action, *status_exchange, *history;
    bool dirty, launching, visible;
    GCancellable *launch_cancel;
    /* Items currently on screen, so a re-render animates only rows that are new. */
    struct shown_item shown[POPUP_ITEMS];
    size_t shown_count;
};
static void start_query(struct popup *popup);
static void set_status(struct popup *popup, const char *text) {
    popup_view_set_status(popup->view, text);
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
    remove_timer(&popup->windows_idle);
    /* Windows change while hidden; the next show reads them again. */
    popup_model_set_windows(popup->model, NULL);
    ipc_exchange_destroy(popup->query);
    popup->query = NULL;
    ipc_exchange_destroy(popup->action);
    popup->action = NULL;
    if (popup->launch_cancel != NULL)
        g_cancellable_cancel(popup->launch_cancel);
    /* Work is canceled above at once; only the window's fade-out remains. */
    popup_view_dismiss(popup->view);
}
/* What a child item says: a window's short title, or the action it offers. */
static void child_text(const struct popup *popup, const tl_popup_item *item, char *text,
                       size_t size) {
    const tl_popup_row *row = popup_model_row(popup->model, item->row);
    const tl_window *window = windows_get(popup->windows, item->window);
    if (item->kind == POPUP_ITEM_WINDOW && window != NULL)
        windows_short_title(window->title, row->name, text, size);
    else if (item->kind == POPUP_ITEM_MORE_WINDOWS) {
        int written = snprintf(text, size, "Show %zu more window%s", item->windows,
                               item->windows == 1 ? "" : "s");
        if (written < 0 || (size_t)written >= size)
            text[0] = 0;
    } else
        g_strlcpy(text, item->kind == POPUP_ITEM_NEW_WINDOW ? "New window" : "", size);
    if (text[0] == 0 && item->kind == POPUP_ITEM_WINDOW)
        g_strlcpy(text, "Untitled window", size);
}
static void select_row(struct popup *popup, bool glide) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(popup->list),
                                                       (int)popup_model_position(popup->model));
    gtk_list_box_select_row(GTK_LIST_BOX(popup->list), row);
    popup_view_select(popup->view, row == NULL ? NULL : GTK_WIDGET(row), glide);
    const tl_popup_item *item = popup_model_selected_item(popup->model);
    char text[WINDOWS_TITLE_BYTES];
    if (item != NULL && item->kind != POPUP_ITEM_RESULT)
        child_text(popup, item, text, sizeof(text));
    if (item != NULL)
        gtk_accessible_announce(
            GTK_ACCESSIBLE(popup->list),
            item->kind == POPUP_ITEM_RESULT ? popup_model_selected(popup->model)->name : text,
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
static tl_popup_entrance entrance_for(const struct popup *popup, const struct shown_item *key) {
    if (popup->shown_count == 0)
        return POPUP_ENTER_CASCADE;
    for (size_t i = 0; i < popup->shown_count; i++)
        if (popup->shown[i].id == key->id && popup->shown[i].handle == key->handle &&
            popup->shown[i].kind == key->kind)
            return POPUP_ENTER_NONE;
    return POPUP_ENTER_FADE;
}
static GtkWidget *item_widget(const struct popup *popup, const tl_popup_item *item,
                              tl_popup_entrance entrance, size_t index) {
    const tl_popup_row *row = popup_model_row(popup->model, item->row);
    if (item->kind == POPUP_ITEM_RESULT)
        return popup_view_result(row, popup_model_distinct_prefix(popup->model, item->row),
                                 entrance, index);
    char text[WINDOWS_TITLE_BYTES];
    child_text(popup, item, text, sizeof(text));
    return popup_view_child(row, item->kind, text, entrance, index);
}
static void render_rows(struct popup *popup) {
    gtk_list_box_remove_all(GTK_LIST_BOX(popup->list));
    size_t count = popup_model_item_count(popup->model);
    struct shown_item keys[POPUP_ITEMS] = {0};
    for (size_t i = 0; i < count; i++) {
        const tl_popup_item *item = popup_model_item(popup->model, i);
        keys[i] = (struct shown_item){popup_model_row(popup->model, item->row)->id, item->handle,
                                      item->kind};
        gtk_list_box_append(GTK_LIST_BOX(popup->list),
                            item_widget(popup, item, entrance_for(popup, &keys[i]), i));
    }
    memcpy(popup->shown, keys, count * sizeof(keys[0]));
    popup->shown_count = count;
}
/* Redraw after windows or expansion changed the list without a new response. */
static void rerender(struct popup *popup, bool glide) {
    render_rows(popup);
    select_row(popup, glide);
}
/* Read the open windows again and list them, keeping the selection. */
static void refresh_windows(struct popup *popup) {
    if (popup->windows == NULL)
        return;
    /* An unreadable window list shows none; results still work. */
    tl_status status = windows_refresh(popup->windows);
    popup_model_set_windows(popup->model, status == TL_OK ? popup->windows : NULL);
    if (popup_model_count(popup->model) != 0)
        rerender(popup, false);
}
static gboolean windows_ready(gpointer context) {
    struct popup *popup = context;
    popup->windows_idle = 0;
    if (popup->visible)
        refresh_windows(popup);
    return G_SOURCE_REMOVE;
}
static void render(struct popup *popup) {
    bool was_empty = popup->shown_count == 0;
    render_rows(popup);
    size_t count = popup_model_count(popup->model);
    popup_view_set_results(popup->view, count);
    popup_view_set_searching(popup->view, false);
    gtk_widget_set_sensitive(popup->list, true);
    select_row(popup, !was_empty);
    if (query_empty(popup))
        set_status(popup, "");
    else if (count == 0)
        set_status(popup, "No matches. Try a name or part of its folder path.");
    else if (popup_model_degraded(popup->model))
        set_status(popup, "Some folders are unavailable");
    else if (popup_model_indexing(popup->model))
        set_status(popup, "Updating index…");
    else {
        /* The list shows how many results there are; only screen readers hear it. */
        set_status(popup, "");
        char message[64];
        int written =
            snprintf(message, sizeof(message), "%zu result%s", count, count == 1 ? "" : "s");
        if (written > 0 && (size_t)written < sizeof(message))
            gtk_accessible_announce(GTK_ACCESSIBLE(popup->list), message,
                                    GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    }
    gtk_widget_set_visible(popup->retry, false);
}
static gboolean pending_status(gpointer context) {
    struct popup *popup = context;
    popup->pending = 0;
    if (popup->visible && (popup->query != NULL || popup->dirty)) {
        set_status(popup, "Searching…");
        popup_view_set_searching(popup->view, true);
    }
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
        popup_view_set_searching(popup->view, false);
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
    popup_view_set_launching(popup->view, false);
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
/* An open event for row, unique per action and tied to the current search. */
static void open_event(const struct popup *popup, const tl_popup_row *row, tl_ipc_request *event) {
    *event = (tl_ipc_request){.operation = IPC_OPEN, .file_id = row->id};
    char *uuid = g_uuid_string_random();
    g_strlcpy(event->event_id, uuid, sizeof(event->event_id));
    g_free(uuid);
    g_strlcpy(event->search_id, popup_model_search_id(popup->model), sizeof(event->search_id));
    g_strlcpy(event->request_id, event->event_id, sizeof(event->request_id));
}
/* Send borrowed event to the daemon's history; replaces an earlier pending send. */
static tl_status record_open(struct popup *popup, const tl_ipc_request *event) {
    ipc_exchange_destroy(popup->history);
    popup->history = NULL;
    return ipc_exchange_create(popup->socket_path, event, history_response, popup, &popup->history);
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
        popup_view_set_launching(popup->view, false);
        if (popup->visible && !canceled)
            set_status(popup,
                       launch->reveal ? "Could not reveal this item" : "Could not open this item");
        return;
    }
    tl_status status = record_open(popup, &launch->event);
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
        popup_view_set_launching(popup->view, false);
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
/* Resolve row, then open, reveal or start a new window of it in the worker. */
static void launch_row(struct popup *popup, const tl_popup_row *row, bool reveal, bool new_window) {
    struct launch *launch = g_try_new0(struct launch, 1);
    if (launch == NULL) {
        set_status(popup, "Could not prepare this action");
        return;
    }
    launch->row = *row;
    launch->reveal = reveal;
    launch->new_window = new_window;
    open_event(popup, row, &launch->event);
    tl_ipc_request request = {.operation = IPC_RESOLVE, .file_id = row->id};
    g_strlcpy(request.request_id, launch->event.request_id, sizeof(request.request_id));
    g_object_set_data_full(G_OBJECT(popup->window), "launch", launch, launch_free);
    /* Acknowledge the choice at once; the fade-out follows an accepted launch. */
    popup_view_set_launching(popup->view, true);
    tl_status status =
        ipc_exchange_create(popup->socket_path, &request, resolved, popup, &popup->action);
    if (status != TL_OK) {
        popup_view_set_launching(popup->view, false);
        set_status(popup, "Could not resolve this item");
    }
}
/* Bring a window of row forward and record it like a launch of row. No resolve
 * is needed: the window exists whatever the desktop catalog now says. */
static void activate_window(struct popup *popup, const tl_popup_row *row, size_t window) {
    if (windows_activate(popup->windows, window) != TL_OK) {
        set_status(popup, "That window has closed");
        refresh_windows(popup);
        return;
    }
    popup_view_set_launching(popup->view, true);
    tl_ipc_request event;
    open_event(popup, row, &event);
    /* The switch already happened; a lost history record must not undo it. */
    if (record_open(popup, &event) != TL_OK)
        set_status(popup, "Opened; history could not be recorded");
    close_popup(popup);
}
static void activate_selected(struct popup *popup, bool reveal) {
    const tl_popup_item *item = popup_model_selected_item(popup->model);
    if (item == NULL || popup->action != NULL || popup->launching)
        return;
    const tl_popup_row *row = popup_model_selected(popup->model);
    if (item->kind == POPUP_ITEM_MORE_WINDOWS) {
        if (popup_model_expand(popup->model))
            rerender(popup, false);
        return;
    }
    /* Enter on a running application switches to its most recent window. */
    if (item->kind == POPUP_ITEM_WINDOW ||
        (item->kind == POPUP_ITEM_RESULT && item->windows != 0 && !reveal)) {
        activate_window(popup, row, item->window);
        return;
    }
    launch_row(popup, row, reveal && item->kind == POPUP_ITEM_RESULT,
               item->kind == POPUP_ITEM_NEW_WINDOW);
}
/* Arrows edit the query unless they can move through the hierarchy: Right at
 * the end of the query lists windows, Left on a child returns to its application. */
static bool hierarchy_key(struct popup *popup, guint key) {
    const tl_popup_item *item = popup_model_selected_item(popup->model);
    if (item == NULL)
        return false;
    if (key == GDK_KEY_Left || key == GDK_KEY_KP_Left) {
        if (item->kind == POPUP_ITEM_RESULT || !popup_model_collapse(popup->model))
            return false;
        rerender(popup, true);
        return true;
    }
    GtkEditable *entry = GTK_EDITABLE(popup->entry);
    int start = 0, end = 0;
    bool at_end =
        !gtk_editable_get_selection_bounds(entry, &start, &end) &&
        gtk_editable_get_position(entry) == (int)g_utf8_strlen(gtk_editable_get_text(entry), -1);
    if (!at_end || !popup_model_expand(popup->model))
        return false;
    rerender(popup, false);
    return true;
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
        select_row(popup, true);
        return true;
    }
    if (key == GDK_KEY_Left || key == GDK_KEY_KP_Left || key == GDK_KEY_Right ||
        key == GDK_KEY_KP_Right)
        return hierarchy_key(popup, key);
    if (key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) {
        if (gtk_widget_get_visible(popup->retry) && popup_model_selected(popup->model) == NULL)
            return gtk_widget_activate(popup->retry);
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
    select_row(popup, true);
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
        popup_view_configure(popup->view, width, area.height * 7 / 10);
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
static tl_status build_window(struct popup *popup) {
    popup->window = gtk_application_window_new(popup->application);
    gtk_window_set_title(GTK_WINDOW(popup->window), "Torchlight");
    gtk_window_set_decorated(GTK_WINDOW(popup->window), false);
    gtk_window_set_resizable(GTK_WINDOW(popup->window), false);
    gtk_window_set_default_size(GTK_WINDOW(popup->window), 680, -1);
    gtk_widget_add_css_class(popup->window, "torchlight");
    tl_status result = popup_view_create(popup->window, &popup->view);
    if (result != TL_OK)
        return result;
    tl_popup_widgets widgets = popup_view_widgets(popup->view);
    popup->entry = widgets.entry;
    popup->list = widgets.list;
    popup->scroll = widgets.scroll;
    popup->retry = widgets.retry;
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
    popup->status_timer = g_timeout_add(POPUP_STATUS_MS, poll_status, popup);
    /* Without an X11 display there is no source and no windows are listed. If
     * even the empty snapshot cannot be allocated, the launcher runs without one. */
    tl_window_source source;
    bool x11 = windows_x11_source(popup->window, &source) == TL_OK;
    if (windows_create(x11 ? &source : NULL, &popup->windows) != TL_OK && x11)
        source.destroy(source.context);
    return TL_OK;
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
    if (popup->window == NULL) {
        tl_status status = build_window(popup);
        if (status != TL_OK) {
            gtk_window_destroy(GTK_WINDOW(popup->window));
            popup->window = NULL;
            g_application_command_line_printerr(command, "Could not build launcher window\n");
            return 1;
        }
    } else if (popup->visible && gtk_window_is_active(GTK_WINDOW(popup->window))) {
        close_popup(popup);
        return 0;
    }
    popup->visible = true;
    popup_view_set_active(popup->view, false);
    gtk_editable_set_text(GTK_EDITABLE(popup->entry), "");
    changed(GTK_EDITABLE(popup->entry), popup);
    /* Clamp before mapping: window managers otherwise constrain an initially
     * oversized scaled window before GTK applies its smaller default size. */
    gtk_widget_realize(popup->window);
    place_window(popup->window, popup);
    gtk_window_present(GTK_WINDOW(popup->window));
    gtk_widget_grab_focus(popup->entry);
    popup_view_set_active(popup->view, true);
    /* Read open windows after the first paint, never on the way to it. */
    remove_timer(&popup->windows_idle);
    popup->windows_idle = g_idle_add(windows_ready, popup);
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
    if (popup->view != NULL)
        close_popup(popup);
    remove_timer(&popup->debounce);
    remove_timer(&popup->pending);
    remove_timer(&popup->status_timer);
    remove_timer(&popup->windows_idle);
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
    /* The X11 source borrows the window, so it goes first. */
    popup_model_set_windows(popup->model, NULL);
    windows_destroy(popup->windows);
    popup->windows = NULL;
    if (popup->window != NULL)
        gtk_window_destroy(GTK_WINDOW(popup->window));
    popup_view_destroy(popup->view);
    g_object_unref(popup->application);
    ipc_path_destroy(popup->socket_path);
    popup_model_destroy(popup->model);
    g_free(popup);
}
