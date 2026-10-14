/* Private window source over KWin scripts, for KDE Plasma 5.27 and 6 on Wayland (ADR 0053):
 * each call loads a short script into KWin over the session bus, which reports the windows,
 * or switches to one, back to an object this source exports on its own bus connection.
 * Needs GIO only. */
#ifndef TORCHLIGHT_GTK_WINDOWS_KWIN_H
#define TORCHLIGHT_GTK_WINDOWS_KWIN_H
#include "torchlight/windows.h"
/* KWin's bus name; that it has an owner tells that KWin runs. */
#define WINDOWS_KWIN_NAME "org.kde.KWin"
/* Longest wait for KWin per list or activation: loading the script, running it and hearing
 * back. KWin answers in about ten milliseconds. */
#define WINDOWS_KWIN_TIMEOUT_MS 250
/** Fill out with a source that asks KWin on the bus at borrowed address (NULL: the session
 * bus) for its normal windows, topmost first (KWin raises a window when it is activated),
 * leaving out the launcher's own, and to activate one. Each call writes a script into
 * borrowed folder (NULL: the user runtime folder), has KWin load and run it, waits at most
 * WINDOWS_KWIN_TIMEOUT_MS for the script's report, which only KWin may send, and unloads it.
 * list returns TL_IO when KWin does not answer or reports nonsense, activate TL_STATE when
 * the window has closed. The source owns its own bus connection, with a private main context
 * that only these calls run, so waiting never runs the caller's main loop. Calls block the
 * thread making them. TL_INVALID for a NULL out, TL_IO when the bus cannot be reached,
 * TL_NOMEM; out zeroed on error. */
tl_status windows_kwin_source(const char *address, const char *folder, tl_window_source *out);
#endif
