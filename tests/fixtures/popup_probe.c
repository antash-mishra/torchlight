/* Observe native GTK frames and entry edits for isolated acceptance measurements. */
#include <X11/Xatom.h>
#include <X11/extensions/XInput2.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <gdk/x11/gdkx.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
struct popup_probe {
    char *filename;
    GtkWidget *entry, *list, *status, *retry, *search, *footer, *scroll, *surface, *preview;
    gint64 changed;
    unsigned retry_clicks;
    bool painted;
};
/** Return the native caller-owned work area, optionally reserving a simulated
 * 40-pixel top panel for the monitor-selection regression. The monitor is
 * borrowed; other calls retain the platform API's behavior and ownership. */
void gdk_x11_monitor_get_workarea(GdkMonitor *monitor, GdkRectangle *area) {
    typedef void (*workarea_function)(GdkMonitor *, GdkRectangle *);
    void *symbol = dlsym(RTLD_NEXT, "gdk_x11_monitor_get_workarea");
    workarea_function workarea = NULL;
    _Static_assert(sizeof(workarea) == sizeof(symbol), "ELF function pointer size");
    memcpy(&workarea, &symbol, sizeof(workarea));
    if (workarea == NULL)
        gdk_monitor_get_geometry(monitor, area);
    else
        workarea(monitor, area);
    if (getenv("TORCHLIGHT_TEST_PANEL") != NULL && area->height > 40) {
        area->y += 40;
        area->height -= 40;
    }
}
/* Pointer queries made for tooltips so far: process-wide, as the X call it counts has no
 * context to carry it. */
static unsigned tooltip_queries;
/* Whether the current call comes from setting a tooltip: GTK then asks where the pointer
 * is, under a grab of the whole X server. GTK's other queries (on focus or window state changes)
 * happen once per change, not per row. */
static bool from_tooltip(void) {
    void *frames[32];
    int count = backtrace(frames, 32);
    for (int i = 0; i < count; i++) {
        Dl_info info;
        if (dladdr(frames[i], &info) != 0 && info.dli_sname != NULL &&
            (strcmp(info.dli_sname, "gtk_widget_set_tooltip_text") == 0 ||
             strcmp(info.dli_sname, "gtk_widget_set_tooltip_markup") == 0 ||
             strcmp(info.dli_sname, "gtk_widget_trigger_tooltip_query") == 0))
            return true;
    }
    return false;
}
/** Count the synchronous pointer queries that tooltips cause, which stalled typing when
 * every new row's tooltip made one; arguments, results and errors pass through unchanged. */
Bool XIQueryPointer(Display *display, int device, Window window, Window *root, Window *child,
                    double *root_x, double *root_y, double *window_x, double *window_y,
                    XIButtonState *buttons, XIModifierState *modifiers, XIGroupState *group) {
    typedef Bool (*query_function)(Display *, int, Window, Window *, Window *, double *, double *,
                                   double *, double *, XIButtonState *, XIModifierState *,
                                   XIGroupState *);
    void *symbol = dlsym(RTLD_NEXT, "XIQueryPointer");
    query_function query = NULL;
    _Static_assert(sizeof(query) == sizeof(symbol), "ELF function pointer size");
    memcpy(&query, &symbol, sizeof(query));
    if (from_tooltip())
        tooltip_queries++;
    if (query == NULL)
        return False;
    return query(display, device, window, root, child, root_x, root_y, window_x, window_y, buttons,
                 modifiers, group);
}
/* When a query request last went to the daemon: process-wide, as the calls that send it
 * carry no popup context. */
static gint64 query_sent_us;
/* Note a query request among bytes the popup sends; its other requests have other ops. */
static void note_query(const void *bytes, size_t length) {
    static const char marker[] = "\"op\":\"query\"";
    if (bytes != NULL && memmem(bytes, length, marker, sizeof(marker) - 1) != NULL)
        query_sent_us = g_get_monotonic_time();
}
/** Note query requests sent with send; arguments, results and errors pass through
 * unchanged. */
ssize_t send(int socket, const void *buffer, size_t length, int flags) {
    typedef ssize_t (*send_function)(int, const void *, size_t, int);
    void *symbol = dlsym(RTLD_NEXT, "send");
    send_function next = NULL;
    _Static_assert(sizeof(next) == sizeof(symbol), "ELF function pointer size");
    memcpy(&next, &symbol, sizeof(next));
    if (next == NULL)
        return -1;
    ssize_t sent = next(socket, buffer, length, flags);
    if (sent > 0)
        note_query(buffer, (size_t)sent);
    return sent;
}
/** Note query requests sent with write; arguments, results and errors pass through
 * unchanged. */
ssize_t write(int file, const void *buffer, size_t length) {
    typedef ssize_t (*write_function)(int, const void *, size_t);
    void *symbol = dlsym(RTLD_NEXT, "write");
    write_function next = NULL;
    _Static_assert(sizeof(next) == sizeof(symbol), "ELF function pointer size");
    memcpy(&next, &symbol, sizeof(next));
    if (next == NULL)
        return -1;
    ssize_t written = next(file, buffer, length);
    if (written > 0)
        note_query(buffer, (size_t)written);
    return written;
}
/** Note query requests sent with sendmsg; arguments, results and errors pass through
 * unchanged. */
ssize_t sendmsg(int socket, const struct msghdr *message, int flags) {
    typedef ssize_t (*sendmsg_function)(int, const struct msghdr *, int);
    void *symbol = dlsym(RTLD_NEXT, "sendmsg");
    sendmsg_function next = NULL;
    _Static_assert(sizeof(next) == sizeof(symbol), "ELF function pointer size");
    memcpy(&next, &symbol, sizeof(next));
    if (next == NULL)
        return -1;
    ssize_t sent = next(socket, message, flags);
    for (size_t i = 0; sent > 0 && message != NULL && i < message->msg_iovlen; i++)
        note_query(message->msg_iov[i].iov_base, message->msg_iov[i].iov_len);
    return sent;
}
/** Delay an already accepted fixture xdg-open to test Escape/reopen while its
 * worker completes. All parameters, output ownership and errors pass through
 * unchanged; ordinary subprocesses and production launch behavior are untouched. */
gboolean g_spawn_async(const gchar *working_directory, gchar **arguments, gchar **environment,
                       GSpawnFlags flags, GSpawnChildSetupFunc child_setup, gpointer setup_data,
                       GPid *child_pid, GError **error) {
    typedef gboolean (*spawn_function)(const gchar *, gchar **, gchar **, GSpawnFlags,
                                       GSpawnChildSetupFunc, gpointer, GPid *, GError **);
    void *symbol = dlsym(RTLD_NEXT, "g_spawn_async");
    spawn_function spawn = NULL;
    _Static_assert(sizeof(spawn) == sizeof(symbol), "ELF function pointer size");
    memcpy(&spawn, &symbol, sizeof(spawn));
    if (spawn == NULL)
        return false;
    gboolean accepted = spawn(working_directory, arguments, environment, flags, child_setup,
                              setup_data, child_pid, error);
    if (accepted && arguments != NULL && arguments[0] != NULL &&
        strcmp(arguments[0], "xdg-open") == 0 && getenv("TORCHLIGHT_TEST_DELAY_OPEN") != NULL)
        g_usleep(400000);
    return accepted;
}
static void probe_destroy(gpointer context) {
    struct popup_probe *probe = context;
    g_free(probe->filename);
    g_free(probe);
}
static GtkWidget *find_widget(GtkWidget *parent, GType type) {
    if (g_type_is_a(G_OBJECT_TYPE(parent), type))
        return parent;
    for (GtkWidget *child = gtk_widget_get_first_child(parent); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_widget(child, type);
        if (found != NULL)
            return found;
    }
    return NULL;
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
/* The first visible label with css_class beneath parent, or NULL. */
static GtkWidget *find_label(GtkWidget *parent, const char *css_class) {
    if (!gtk_widget_get_visible(parent))
        return NULL;
    if (GTK_IS_LABEL(parent) && gtk_widget_has_css_class(parent, css_class))
        return parent;
    for (GtkWidget *child = gtk_widget_get_first_child(parent); child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        GtkWidget *found = find_label(child, css_class);
        if (found != NULL)
            return found;
    }
    return NULL;
}
static void entry_changed(GtkEditable *entry, gpointer context) {
    (void)entry;
    struct popup_probe *probe = context;
    probe->changed = g_get_monotonic_time();
}
static void retry_clicked(GtkButton *button, gpointer context) {
    struct popup_probe *probe = context;
    probe->retry_clicks++;
    gdk_frame_clock_request_phase(gtk_widget_get_frame_clock(GTK_WIDGET(button)),
                                  GDK_FRAME_CLOCK_PHASE_PAINT);
}
/* The bounding box of the window's _NET_WM_OPAQUE_REGION in logical pixels, as left, top,
 * right and bottom; all zero when it has none or is not an X11 window. */
static void opaque_extent(GtkWidget *window, int extent[4]) {
    memset(extent, 0, 4 * sizeof *extent);
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
    if (surface == NULL || !GDK_IS_X11_SURFACE(surface))
        return;
    Display *display = gdk_x11_display_get_xdisplay(gdk_surface_get_display(surface));
    Atom type;
    int format;
    unsigned long count, remaining;
    unsigned char *data = NULL;
    if (XGetWindowProperty(display, gdk_x11_surface_get_xid(surface),
                           XInternAtom(display, "_NET_WM_OPAQUE_REGION", False), 0, 1024, False,
                           XA_CARDINAL, &type, &format, &count, &remaining, &data) != Success)
        return;
    /* Format-32 properties arrive as longs: x, y, width and height per rectangle. */
    const long *values = (const long *)(void *)data;
    long scale = gtk_widget_get_scale_factor(window), box[4] = {0, 0, 0, 0};
    for (unsigned long i = 0; data != NULL && format == 32 && i + 3 < count; i += 4) {
        long left = values[i], top = values[i + 1];
        long right = left + values[i + 2], bottom = top + values[i + 3];
        bool first = i == 0;
        box[0] = first || left < box[0] ? left : box[0];
        box[1] = first || top < box[1] ? top : box[1];
        box[2] = first || right > box[2] ? right : box[2];
        box[3] = first || bottom > box[3] ? bottom : box[3];
    }
    for (int i = 0; i < 4; i++)
        extent[i] = (int)(box[i] / scale);
    if (data != NULL)
        XFree(data);
}
static void write_frame(struct popup_probe *probe, bool ready) {
    gint64 painted = g_get_monotonic_time();
    const char *query = gtk_editable_get_text(GTK_EDITABLE(probe->entry));
    char *encoded_query = g_base64_encode((const guchar *)query, strlen(query));
    const char *status = gtk_label_get_text(GTK_LABEL(probe->status));
    char *encoded_status = g_base64_encode((const guchar *)status, strlen(status));
    GtkWidget *preview_name = find_label(probe->preview, "preview-name");
    const char *name = preview_name != NULL ? gtk_label_get_text(GTK_LABEL(preview_name)) : "";
    char *encoded_name = g_base64_encode((const guchar *)name, strlen(name));
    size_t rows = 0;
    for (GtkWidget *child = gtk_widget_get_first_child(probe->list); child != NULL;
         child = gtk_widget_get_next_sibling(child))
        if (GTK_IS_LIST_BOX_ROW(child))
            rows++;
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(probe->list));
    GtkWidget *window = GTK_WIDGET(gtk_widget_get_root(probe->entry));
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, 0, 0), surface = bounds;
    if (!gtk_widget_compute_bounds(probe->entry, window, &bounds) ||
        !gtk_widget_compute_bounds(probe->surface, window, &surface))
        return;
    GdkDisplay *display = gtk_widget_get_display(window);
    bool clear = gdk_display_is_composited(display) && gdk_display_is_rgba(display);
    int opaque[4];
    opaque_extent(window, opaque);
    FILE *file = fopen(probe->filename, "a");
    if (file != NULL) {
        int written = fprintf(
            file,
            "{\"paint_us\":%lld,\"changed_us\":%lld,\"first\":%s,"
            "\"ready\":%s,\"rows\":%zu,\"query_b64\":\"%s\","
            "\"status_b64\":\"%s\",\"retry\":%s,"
            "\"retry_focus\":%s,\"retry_clicks\":%u,\"selected\":%d,"
            "\"results_visible\":%s,\"footer_visible\":%s,\"typing\":%s,"
            "\"entry_x\":%.1f,\"entry_y\":%.1f,\"entry_width\":%.1f,\"clear_icon\":%s,"
            "\"shadow_left\":%d,\"shadow_top\":%d,\"preview_visible\":%s,"
            "\"preview_name_b64\":\"%s\",\"surface_width\":%.1f,\"surface_height\":%.1f,"
            "\"clear\":%s,\"opaque\":[%d,%d,%d,%d],\"tooltip_queries\":%u,\"query_sent_us\":%lld}"
            "\n",
            (long long)painted, (long long)probe->changed, probe->painted ? "false" : "true",
            ready ? "true" : "false", rows, encoded_query, encoded_status,
            gtk_widget_get_visible(probe->retry) ? "true" : "false",
            gtk_widget_has_focus(probe->retry) ? "true" : "false", probe->retry_clicks,
            selected == NULL ? -1 : gtk_list_box_row_get_index(selected),
            gtk_widget_get_visible(probe->scroll) ? "true" : "false",
            gtk_widget_get_visible(probe->footer) ? "true" : "false",
            gtk_widget_has_css_class(probe->search, "typing") ? "true" : "false",
            (double)bounds.origin.x, (double)bounds.origin.y, (double)bounds.size.width,
            gtk_entry_get_icon_storage_type(GTK_ENTRY(probe->entry), GTK_ENTRY_ICON_SECONDARY) ==
                    GTK_IMAGE_EMPTY
                ? "false"
                : "true",
            gtk_widget_get_margin_start(probe->surface), gtk_widget_get_margin_top(probe->surface),
            gtk_widget_get_visible(probe->preview) ? "true" : "false", encoded_name,
            (double)surface.size.width, (double)surface.size.height, clear ? "true" : "false",
            opaque[0], opaque[1], opaque[2], opaque[3], tooltip_queries, (long long)query_sent_us);
        if (written < 0)
            fputs("Could not record popup frame\n", stderr);
        if (fclose(file) != 0)
            fputs("Could not close popup frame log\n", stderr);
    }
    g_free(encoded_query);
    g_free(encoded_status);
    g_free(encoded_name);
}
static void after_paint(GdkFrameClock *clock, gpointer context) {
    (void)clock;
    GtkWindow *window = context;
    struct popup_probe *probe = g_object_get_data(G_OBJECT(window), "torchlight-test-probe");
    bool ready = gtk_widget_get_sensitive(probe->list);
    /* Measure when the current rows have actually painted. Keep supplementary
     * error/status frames too so recovery checks can inspect the native view. */
    write_frame(probe, ready);
    probe->painted = true;
}
/** Present a caller-owned window, attaching an observer only when the fixture
 * log environment variable is set. Keeps native behavior and ownership; a failed
 * observer allocation/read never becomes a production launch dependency. */
void gtk_window_present(GtkWindow *window) {
    typedef void (*present_function)(GtkWindow *);
    void *symbol = dlsym(RTLD_NEXT, "gtk_window_present");
    present_function present = NULL;
    _Static_assert(sizeof(present) == sizeof(symbol), "ELF function pointer size");
    memcpy(&present, &symbol, sizeof(present));
    if (present == NULL)
        return;
    present(window);
    const char *filename = getenv("TORCHLIGHT_TEST_POPUP_FRAMES");
    if (filename == NULL || g_object_get_data(G_OBJECT(window), "torchlight-test-probe") != NULL)
        return;
    struct popup_probe *probe = g_new0(struct popup_probe, 1);
    probe->filename = g_strdup(filename);
    probe->entry = find_widget(GTK_WIDGET(window), GTK_TYPE_ENTRY);
    probe->list = find_widget(GTK_WIDGET(window), GTK_TYPE_LIST_BOX);
    probe->status = find_named(GTK_WIDGET(window), "popup-status");
    probe->retry = find_named(GTK_WIDGET(window), "popup-retry");
    probe->search = find_named(GTK_WIDGET(window), "popup-search");
    probe->footer = find_named(GTK_WIDGET(window), "popup-footer");
    probe->scroll = find_named(GTK_WIDGET(window), "popup-results");
    probe->surface = find_named(GTK_WIDGET(window), "popup-surface");
    probe->preview = find_named(GTK_WIDGET(window), "popup-preview");
    GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(window));
    if (probe->entry == NULL || probe->list == NULL || !GTK_IS_LABEL(probe->status) ||
        probe->retry == NULL || probe->search == NULL || probe->footer == NULL ||
        probe->scroll == NULL || probe->surface == NULL || probe->preview == NULL ||
        clock == NULL) {
        probe_destroy(probe);
        return;
    }
    g_object_set_data_full(G_OBJECT(window), "torchlight-test-probe", probe, probe_destroy);
    g_signal_connect(probe->entry, "changed", G_CALLBACK(entry_changed), probe);
    g_signal_connect(probe->retry, "clicked", G_CALLBACK(retry_clicked), probe);
    g_signal_connect_object(clock, "after-paint", G_CALLBACK(after_paint), window, 0);
}
