/* Choosing the open-window source for the session the popup runs in (ADR 0052): X11 when
 * the display is X11; a compositor's task bar protocol when it offers one (the wlroots
 * family, COSMIC); otherwise the desktop's own interface: Torchlight's GNOME Shell or
 * Cinnamon extension, or KWin scripts. Turns the shell extensions on once per person. */
#ifndef TORCHLIGHT_GTK_WINDOWS_SESSION_H
#define TORCHLIGHT_GTK_WINDOWS_SESSION_H
#include "torchlight/windows.h"
#include <gtk/gtk.h>
/** Fill out with the open-window source for the session that borrowed popup (a GtkWindow,
 * shown or not) runs in. When that is Torchlight's GNOME Shell or Cinnamon extension, also
 * turn the extension on once for the person, with markers under borrowed state_dir. A desktop
 * is recognised by its bus name, or by XDG_CURRENT_DESKTOP while its shell has not started
 * yet. Main thread only; may wait a few hundred milliseconds for the desktop shell.
 * TL_INVALID for NULL arguments, TL_STATE when the session offers no source, other errors
 * from the source chosen; out zeroed on error. */
tl_status windows_session_source(GtkWidget *popup, const char *state_dir, tl_window_source *out);
#endif
