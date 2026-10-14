/* Private GNOME Shell window source: on GNOME's Wayland session, Torchlight's GNOME Shell
 * extension lists and activates windows over the session bus (ADR 0051). Needs GIO only. */
#ifndef TORCHLIGHT_GTK_WINDOWS_GNOME_H
#define TORCHLIGHT_GTK_WINDOWS_GNOME_H
#include "torchlight/windows.h"
#include <gio/gio.h>
#include <stdbool.h>
/* The extension's UUID: its folder under share/gnome-shell/extensions. */
#define WINDOWS_GNOME_EXTENSION "torchlight@antash-mishra.github.io"
/* Longest wait for GNOME Shell per call; a healthy shell answers in a few milliseconds. */
#define WINDOWS_GNOME_TIMEOUT_MS 250
/** Whether borrowed desktop (an XDG_CURRENT_DESKTOP value, colon-separated, may be NULL)
 * names GNOME. No errors. */
bool windows_gnome_desktop(const char *desktop);
/** Fill out with a source that asks Torchlight's GNOME Shell extension on bus (NULL: the
 * session bus) for GNOME Shell's windows, most recently used first, leaving out the
 * launcher's own, and to activate one. Each call waits at most WINDOWS_GNOME_TIMEOUT_MS;
 * list returns TL_IO while the extension is not running and activate TL_STATE for a closed
 * window. The source holds a bus reference released by its destroy. Calls block the thread
 * making them. TL_INVALID for a NULL out, TL_IO when there is no session bus; out zeroed on
 * error. */
tl_status windows_gnome_source(GDBusConnection *bus, tl_window_source *out);
/** Enable the extension once for this user, through GNOME Shell on bus (NULL: the session
 * bus): when the shell has it installed but not enabled and allows user extensions, ask it
 * to enable it; once enabled (by us or already), create borrowed marker and its folders.
 * Later calls see marker and do nothing, so a person's own later disable stands. Also does
 * nothing while the shell is not running or has not found the extension (GNOME Shell looks
 * for new extensions at login). Blocks for at most three WINDOWS_GNOME_TIMEOUT_MS waits. No
 * errors: a failure leaves the extension as it was. */
void windows_gnome_enable_once(GDBusConnection *bus, const char *marker);
#endif
