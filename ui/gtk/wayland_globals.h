/* Reading which Wayland globals, at which versions, the compositor offers, for working around
 * compositor differences GTK does not hide. Needs libwayland-client, which GTK already uses. */
#ifndef TORCHLIGHT_GTK_WAYLAND_GLOBALS_H
#define TORCHLIGHT_GTK_WAYLAND_GLOBALS_H
#include <gtk/gtk.h>
#include <stdint.h>
/** The version at which the compositor of borrowed display offers the global named by
 * borrowed interface (such as "gtk_shell1"), or 0 when it does not, display is not Wayland or
 * an argument is NULL. Reads the globals with one round trip on a private queue, so that
 * GTK's own events are not dispatched under it. Main thread only. No errors: failures read
 * as 0. */
uint32_t wayland_global_version(GdkDisplay *display, const char *interface);
#endif
