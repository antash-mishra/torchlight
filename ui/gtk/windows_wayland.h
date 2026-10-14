/* Private Wayland window source over a compositor's task bar protocol, on GTK's own Wayland
 * connection (ADR 0052): the wlroots foreign-toplevel manager (Sway, Hyprland, labwc,
 * Wayfire, niri) or COSMIC's toplevel info and manager over the standard foreign-toplevel
 * list. Needs libwayland-client, which GTK already uses. */
#ifndef TORCHLIGHT_GTK_WINDOWS_WAYLAND_H
#define TORCHLIGHT_GTK_WINDOWS_WAYLAND_H
#include "torchlight/windows.h"
#include <gtk/gtk.h>
/** Fill out with a source for the open windows that the compositor of borrowed display
 * reports: listed most recently used first (by the activations seen since the source was
 * created, then newest first), leaving out the launcher's own, and activated with the
 * display's default seat. Creating it reads the compositor's current windows with a few
 * round trips on a private queue; after that their events arrive through GTK's own event
 * dispatch, so list and activate never wait. The source owns a display reference released by
 * its destroy. Main thread only. TL_INVALID for NULL arguments, TL_STATE when display is not
 * Wayland or its compositor offers neither protocol, TL_IO when the connection fails,
 * TL_NOMEM; out zeroed on error. */
tl_status windows_wayland_source(GdkDisplay *display, tl_window_source *out);
#endif
