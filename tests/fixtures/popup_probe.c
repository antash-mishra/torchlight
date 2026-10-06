/* Observe native GTK frames and entry edits for isolated acceptance measurements. */
#include <dlfcn.h>
#include <gdk/x11/gdkx.h>
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct popup_probe {
    char *filename;
    GtkWidget *entry, *list, *status, *retry, *search, *footer, *scroll;
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
static void write_frame(struct popup_probe *probe, bool ready) {
    gint64 painted = g_get_monotonic_time();
    const char *query = gtk_editable_get_text(GTK_EDITABLE(probe->entry));
    char *encoded_query = g_base64_encode((const guchar *)query, strlen(query));
    const char *status = gtk_label_get_text(GTK_LABEL(probe->status));
    char *encoded_status = g_base64_encode((const guchar *)status, strlen(status));
    size_t rows = 0;
    for (GtkWidget *child = gtk_widget_get_first_child(probe->list); child != NULL;
         child = gtk_widget_get_next_sibling(child))
        if (GTK_IS_LIST_BOX_ROW(child))
            rows++;
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(probe->list));
    GtkWidget *window = GTK_WIDGET(gtk_widget_get_root(probe->entry));
    graphene_rect_t bounds = GRAPHENE_RECT_INIT(0, 0, 0, 0);
    if (!gtk_widget_compute_bounds(probe->entry, window, &bounds))
        return;
    FILE *file = fopen(probe->filename, "a");
    if (file != NULL) {
        int written = fprintf(
            file,
            "{\"paint_us\":%lld,\"changed_us\":%lld,\"first\":%s,"
            "\"ready\":%s,\"rows\":%zu,\"query_b64\":\"%s\","
            "\"status_b64\":\"%s\",\"retry\":%s,"
            "\"retry_focus\":%s,\"retry_clicks\":%u,\"selected\":%d,"
            "\"results_visible\":%s,\"footer_visible\":%s,\"typing\":%s,"
            "\"entry_x\":%.1f,\"entry_y\":%.1f,\"entry_width\":%.1f,\"clear_icon\":%s}\n",
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
                : "true");
        if (written < 0)
            fputs("Could not record popup frame\n", stderr);
        if (fclose(file) != 0)
            fputs("Could not close popup frame log\n", stderr);
    }
    g_free(encoded_query);
    g_free(encoded_status);
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
    GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(window));
    if (probe->entry == NULL || probe->list == NULL || !GTK_IS_LABEL(probe->status) ||
        probe->retry == NULL || probe->search == NULL || probe->footer == NULL ||
        probe->scroll == NULL || clock == NULL) {
        probe_destroy(probe);
        return;
    }
    g_object_set_data_full(G_OBJECT(window), "torchlight-test-probe", probe, probe_destroy);
    g_signal_connect(probe->entry, "changed", G_CALLBACK(entry_changed), probe);
    g_signal_connect(probe->retry, "clicked", G_CALLBACK(retry_clicked), probe);
    g_signal_connect_object(clock, "after-paint", G_CALLBACK(after_paint), window, 0);
}
