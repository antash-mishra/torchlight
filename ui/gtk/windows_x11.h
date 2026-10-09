/* Private X11 window source for the popup: EWMH listing and activation on GTK's connection. */
#ifndef TORCHLIGHT_GTK_WINDOWS_X11_H
#define TORCHLIGHT_GTK_WINDOWS_X11_H
#include "torchlight/windows.h"
#include <gtk/gtk.h>
/** Fill out with a source listing the window manager's managed windows on the
 * display of borrowed popup, which must outlive the source and is never listed.
 * The source owns a display reference released by its destroy. Main thread
 * only, since it uses GTK's own X connection under GDK error traps; reading
 * costs a few round trips per window and never waits on anything else.
 * TL_INVALID for NULL arguments, TL_STATE when the display is not X11 (or GTK
 * lacks X11 support), TL_NOMEM; out zeroed on error. */
tl_status windows_x11_source(GtkWidget *popup, tl_window_source *out);
#endif
