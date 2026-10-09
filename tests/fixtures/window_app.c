/* X11 fixture application: maps one window per title with a given WM_CLASS,
 * prints each window id on its own line once mapped, then waits to be killed. */
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>
#define WINDOW_SIZE 320
#define WINDOW_OFFSET 40
#define FIRST_TITLE 3
int main(int argc, char **argv) {
    if (argc <= FIRST_TITLE) {
        fputs("usage: test_window_app INSTANCE CLASS TITLE...\n", stderr);
        return 2;
    }
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        fputs("test_window_app: cannot open display\n", stderr);
        return 1;
    }
    Atom name = XInternAtom(display, "_NET_WM_NAME", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    XClassHint hint = {argv[1], argv[2]};
    for (int i = FIRST_TITLE; i < argc; i++) {
        Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), WINDOW_OFFSET * i,
                                            WINDOW_OFFSET * i, WINDOW_SIZE, WINDOW_SIZE, 0, 0, 0);
        XSetClassHint(display, window, &hint);
        XStoreName(display, window, argv[i]);
        XChangeProperty(display, window, name, utf8, 8, PropModeReplace,
                        (const unsigned char *)argv[i], (int)strlen(argv[i]));
        /* Map in order and wait each time, so the last title ends on top: the
         * most recently used window. */
        XMapWindow(display, window);
        XSync(display, False);
        printf("%lu\n", (unsigned long)window);
        if (fflush(stdout) != 0)
            return 1;
    }
    for (;;) {
        XEvent event;
        XNextEvent(display, &event);
    }
}
