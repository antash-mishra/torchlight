/* EWMH window listing and activation on GTK's own X11 connection, under GDK error traps. */
#include "windows_x11.h"
#include <string.h>
#ifdef GDK_WINDOWING_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gdk/x11/gdkx.h>
/* Managed windows read from the window manager's list; later ones are ignored. */
#define X11_CLIENT_LIMIT 1024
/* Window types and states read per window; real windows carry a handful. */
#define X11_ATOM_LIMIT 32
/* EWMH source indication for pagers and task lists: the window manager treats
 * the request as the user's, raising the window and switching workspace. */
#define X11_SOURCE_PAGER 2
/* XGetWindowProperty lengths count 32-bit units. */
#define X11_UNIT_BYTES 4
enum {
    ATOM_STACKING,
    ATOM_CLIENTS,
    ATOM_TYPE,
    ATOM_TYPE_NORMAL,
    ATOM_STATE,
    ATOM_SKIP_TASKBAR,
    ATOM_NAME,
    ATOM_UTF8,
    ATOM_GTK_ID,
    ATOM_ACTIVE,
    ATOM_COUNT
};
static const char *const ATOM_NAMES[ATOM_COUNT] = {"_NET_CLIENT_LIST_STACKING",
                                                   "_NET_CLIENT_LIST",
                                                   "_NET_WM_WINDOW_TYPE",
                                                   "_NET_WM_WINDOW_TYPE_NORMAL",
                                                   "_NET_WM_STATE",
                                                   "_NET_WM_STATE_SKIP_TASKBAR",
                                                   "_NET_WM_NAME",
                                                   "UTF8_STRING",
                                                   "_GTK_APPLICATION_ID",
                                                   "_NET_ACTIVE_WINDOW"};
struct x11_source {
    GdkDisplay *display;
    GtkWidget *popup;
    Atom atoms[ATOM_COUNT];
};
/* Read up to units 32-bit units of a property with the given type and format.
 * Returns XFree-owned data or NULL; truncated reports a longer value. */
static unsigned char *property(Display *display, Window window, Atom name, Atom type, int format,
                               long units, unsigned long *count, bool *truncated) {
    Atom actual = None;
    int actual_format = 0;
    unsigned long after = 0;
    unsigned char *data = NULL;
    *count = 0;
    if (XGetWindowProperty(display, window, name, 0, units, False, type, &actual, &actual_format,
                           count, &after, &data) != Success)
        return NULL;
    if (data != NULL && (actual != type || actual_format != format || *count == 0)) {
        XFree(data);
        data = NULL;
    }
    if (data == NULL)
        *count = 0;
    if (truncated != NULL)
        *truncated = after != 0;
    return data;
}
/* Managed windows from bottom to top, or in mapping order when the window
 * manager publishes no stacking list. Free with XFree; NULL when there is none. */
static Window *client_list(const struct x11_source *source, Display *display,
                           unsigned long *count) {
    Window root = DefaultRootWindow(display);
    for (int list = ATOM_STACKING; list <= ATOM_CLIENTS; list++) {
        unsigned char *data = property(display, root, source->atoms[list], XA_WINDOW, 32,
                                       X11_CLIENT_LIMIT, count, NULL);
        if (data != NULL)
            /* Format-32 properties arrive as arrays of long, the size of Window. */
            return (Window *)(void *)data;
    }
    return NULL;
}
static bool has_atom(const Atom *atoms, unsigned long count, Atom wanted) {
    for (unsigned long i = 0; i < count; i++)
        if (atoms[i] == wanted)
            return true;
    return false;
}
/* Ordinary application windows: normal type (an untyped window counts unless it
 * is transient, which EWMH treats as a dialog) and shown in task lists. */
static bool listable(const struct x11_source *source, Display *display, Window window) {
    unsigned long count = 0;
    Atom *types = (Atom *)(void *)property(display, window, source->atoms[ATOM_TYPE], XA_ATOM, 32,
                                           X11_ATOM_LIMIT, &count, NULL);
    Window parent = None;
    bool normal = types != NULL ? types[0] == source->atoms[ATOM_TYPE_NORMAL]
                                : !XGetTransientForHint(display, window, &parent);
    if (types != NULL)
        XFree(types);
    if (!normal)
        return false;
    Atom *states = (Atom *)(void *)property(display, window, source->atoms[ATOM_STATE], XA_ATOM, 32,
                                            X11_ATOM_LIMIT, &count, NULL);
    bool skipped = states != NULL && has_atom(states, count, source->atoms[ATOM_SKIP_TASKBAR]);
    if (states != NULL)
        XFree(states);
    return !skipped;
}
/* Copy an exact identifier, or leave it empty when it does not fit. */
static void copy_name(char *out, size_t capacity, const char *name) {
    out[0] = 0;
    if (name != NULL && strlen(name) < capacity)
        memcpy(out, name, strlen(name) + 1);
}
/* Copy text as valid single-line UTF-8, controls as spaces, ending on a whole character. */
static void copy_text(char *out, size_t capacity, const char *bytes, size_t length,
                      bool truncated) {
    /* A value cut by the read limit may end inside a character: drop that character. */
    if (truncated) {
        while (length > 0 && ((unsigned char)bytes[length - 1] & 0xC0) == 0x80)
            length--;
        if (length > 0)
            length--;
    }
    char *valid = g_utf8_make_valid(bytes, (gssize)length);
    size_t used = 0;
    for (const char *p = valid; *p != 0; p = g_utf8_next_char(p)) {
        size_t size = (size_t)(g_utf8_next_char(p) - p);
        if (used + size >= capacity)
            break;
        if (g_unichar_iscntrl(g_utf8_get_char(p)))
            out[used++] = ' ';
        else {
            memcpy(out + used, p, size);
            used += size;
        }
    }
    out[used] = 0;
    g_free(valid);
}
/* _NET_WM_NAME, else a Latin-1 WM_NAME converted to UTF-8; empty when neither exists. */
static void read_title(const struct x11_source *source, Display *display, Window window,
                       char *out) {
    const long units = WINDOWS_TITLE_BYTES / X11_UNIT_BYTES;
    unsigned long count = 0;
    bool truncated = false;
    out[0] = 0;
    unsigned char *data = property(display, window, source->atoms[ATOM_NAME],
                                   source->atoms[ATOM_UTF8], 8, units, &count, &truncated);
    if (data != NULL) {
        copy_text(out, WINDOWS_TITLE_BYTES, (const char *)data, count, truncated);
        XFree(data);
        return;
    }
    data = property(display, window, XA_WM_NAME, XA_STRING, 8, units, &count, NULL);
    if (data == NULL)
        return;
    char *converted =
        g_convert((const char *)data, (gssize)count, "UTF-8", "ISO-8859-1", NULL, NULL, NULL);
    XFree(data);
    if (converted != NULL)
        copy_text(out, WINDOWS_TITLE_BYTES, converted, strlen(converted), false);
    g_free(converted);
}
static bool read_window(const struct x11_source *source, Display *display, Window window,
                        tl_window *out) {
    if (!listable(source, display, window))
        return false;
    *out = (tl_window){.handle = window};
    XClassHint hint = {0};
    if (XGetClassHint(display, window, &hint)) {
        copy_name(out->instance, sizeof(out->instance), hint.res_name);
        copy_name(out->class_name, sizeof(out->class_name), hint.res_class);
        if (hint.res_name != NULL)
            XFree(hint.res_name);
        if (hint.res_class != NULL)
            XFree(hint.res_class);
    }
    unsigned long count = 0;
    bool truncated = false;
    unsigned char *id =
        property(display, window, source->atoms[ATOM_GTK_ID], source->atoms[ATOM_UTF8], 8,
                 WINDOWS_CLASS_BYTES / X11_UNIT_BYTES, &count, &truncated);
    if (id != NULL && !truncated && count < sizeof(out->app_id) && memchr(id, 0, count) == NULL)
        memcpy(out->app_id, id, count + 1);
    if (id != NULL)
        XFree(id);
    read_title(source, display, window, out->title);
    return true;
}
static Window popup_window(const struct x11_source *source) {
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(source->popup));
    return surface != NULL && GDK_IS_X11_SURFACE(surface) ? gdk_x11_surface_get_xid(surface) : None;
}
static tl_status x11_list(void *context, tl_window *out, size_t capacity, size_t *count) {
    const struct x11_source *source = context;
    Display *display = gdk_x11_display_get_xdisplay(source->display);
    Window self = popup_window(source);
    *count = 0;
    gdk_x11_display_error_trap_push(source->display);
    unsigned long clients = 0;
    Window *stack = client_list(source, display, &clients);
    /* Top of the stack first: the most recently used window leads. */
    for (unsigned long i = clients; i-- > 0 && *count < capacity;)
        if (stack[i] != self && read_window(source, display, stack[i], &out[*count]))
            (*count)++;
    if (stack != NULL)
        XFree(stack);
    /* Windows destroyed while being read raise errors that only drop them. */
    gdk_x11_display_error_trap_pop_ignored(source->display);
    return TL_OK;
}
static tl_status x11_activate(void *context, uint64_t handle) {
    const struct x11_source *source = context;
    Display *display = gdk_x11_display_get_xdisplay(source->display);
    Window window = (Window)handle;
    if ((uint64_t)window != handle)
        return TL_INVALID;
    gdk_x11_display_error_trap_push(source->display);
    unsigned long clients = 0;
    Window *stack = client_list(source, display, &clients);
    bool managed = false;
    for (unsigned long i = 0; i < clients; i++)
        managed = managed || stack[i] == window;
    if (stack != NULL)
        XFree(stack);
    if (managed) {
        XEvent event = {0};
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.message_type = source->atoms[ATOM_ACTIVE];
        event.xclient.format = 32;
        event.xclient.data.l[0] = X11_SOURCE_PAGER;
        /* The key or click that chose the window, so focus-stealing checks pass. */
        event.xclient.data.l[1] = (long)gdk_x11_display_get_user_time(source->display);
        XSendEvent(display, DefaultRootWindow(display), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XFlush(display);
    }
    int error = gdk_x11_display_error_trap_pop(source->display);
    return managed && error == 0 ? TL_OK : TL_STATE;
}
static void x11_destroy(void *context) {
    struct x11_source *source = context;
    if (source == NULL)
        return;
    g_object_unref(source->display);
    g_free(source);
}
tl_status windows_x11_source(GtkWidget *popup, tl_window_source *out) {
    if (out == NULL || popup == NULL)
        return TL_INVALID;
    *out = (tl_window_source){0};
    GdkDisplay *display = gtk_widget_get_display(popup);
    if (!GDK_IS_X11_DISPLAY(display))
        return TL_STATE;
    struct x11_source *source = g_try_new0(struct x11_source, 1);
    if (source == NULL)
        return TL_NOMEM;
    source->display = g_object_ref(display);
    source->popup = popup;
    /* GDK caches atoms, so this costs round trips only on first use. */
    for (size_t i = 0; i < ATOM_COUNT; i++)
        source->atoms[i] = gdk_x11_get_xatom_by_name_for_display(display, ATOM_NAMES[i]);
    *out = (tl_window_source){source, x11_list, x11_activate, x11_destroy};
    return TL_OK;
}
#else
tl_status windows_x11_source(GtkWidget *popup, tl_window_source *out) {
    (void)popup;
    if (out == NULL)
        return TL_INVALID;
    *out = (tl_window_source){0};
    return TL_STATE;
}
#endif
