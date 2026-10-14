/* Window source fixture: picks the open-window source for the session it runs in, as the
 * popup does (windows_session_source), and lists its windows, most recently used first, one
 * per line as "handle<TAB>app id<TAB>class<TAB>instance<TAB>title". With a title it then
 * switches to that window, waits for the desktop to report the switch, and lists again after
 * a "--" line. Exits 3 when the session offers no source, 4 when the window is missing or
 * cannot be activated. */
#include "../../ui/gtk/windows_session.h"
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
/* Time for the desktop to switch windows and say so, as the popup's next showing would. */
#define SETTLE_MS 600
static void print_windows(tl_windows *windows) {
    for (size_t i = 0; i < windows_count(windows); i++) {
        const tl_window *window = windows_get(windows, i);
        printf("%llu\t%s\t%s\t%s\t%s\n", (unsigned long long)window->handle, window->app_id,
               window->class_name, window->instance, window->title);
    }
    fflush(stdout);
}
static gboolean settled(gpointer data) {
    *(bool *)data = true;
    return G_SOURCE_REMOVE;
}
/* Run GTK's main context for a while, so that compositor events arrive. */
static void settle(void) {
    bool done = false;
    g_timeout_add(SETTLE_MS, settled, &done);
    while (!done)
        g_main_context_iteration(NULL, true);
}
static int activate(tl_windows *windows, const char *title) {
    for (size_t i = 0; i < windows_count(windows); i++)
        if (strcmp(windows_get(windows, i)->title, title) == 0) {
            if (windows_activate(windows, i) != TL_OK)
                return 4;
            settle();
            puts("--");
            if (windows_refresh(windows) != TL_OK)
                return 4;
            print_windows(windows);
            return 0;
        }
    fprintf(stderr, "test_window_sources: no window titled %s\n", title);
    return 4;
}
int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fputs("usage: test_window_sources STATE_DIR [TITLE]\n", stderr);
        return 2;
    }
    gtk_init();
    /* Never shown: the X11 source only uses it to leave the popup out. */
    GtkWidget *popup = gtk_window_new();
    tl_window_source source;
    tl_windows *windows = NULL;
    if (windows_session_source(popup, argv[1], &source) != TL_OK) {
        fputs("test_window_sources: this session offers no window source\n", stderr);
        return 3;
    }
    if (windows_create(&source, &windows) != TL_OK) {
        source.destroy(source.context);
        return 1;
    }
    /* Events already sent by the desktop arrive before the first list. */
    settle();
    int status = windows_refresh(windows) == TL_OK ? 0 : 1;
    if (status == 0)
        print_windows(windows);
    if (status == 0 && argc == 3)
        status = activate(windows, argv[2]);
    windows_destroy(windows);
    gtk_window_destroy(GTK_WINDOW(popup));
    return status;
}
