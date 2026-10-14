/* Private window source over Torchlight's desktop shell extension, for GNOME Shell (ADR 0051)
 * and its fork Cinnamon (ADR 0054) on Wayland: the extension lists and activates windows over
 * the session bus. Also enables the extension once per user, and tells which shell runs.
 * Needs GIO only. */
#ifndef TORCHLIGHT_GTK_WINDOWS_SHELL_H
#define TORCHLIGHT_GTK_WINDOWS_SHELL_H
#include "torchlight/windows.h"
#include <gio/gio.h>
#include <stdbool.h>
/* The extension's UUID, the same for both shells: its folder under
 * share/gnome-shell/extensions and share/cinnamon/extensions. */
#define WINDOWS_SHELL_EXTENSION "torchlight@antash-mishra.github.io"
/* The shells' bus names; the extension serves its interface on its shell's name. */
#define WINDOWS_SHELL_GNOME "org.gnome.Shell"
#define WINDOWS_SHELL_CINNAMON "org.Cinnamon"
/* Longest wait for a shell per call; a healthy shell answers in a few milliseconds. */
#define WINDOWS_SHELL_TIMEOUT_MS 250
/** Whether borrowed desktops (an XDG_CURRENT_DESKTOP value, colon-separated, may be NULL)
 * includes borrowed name, ignoring ASCII case. No errors. */
bool windows_shell_desktop(const char *desktops, const char *name);
/** Whether borrowed name has an owner on bus (NULL: the session bus), waiting at most
 * WINDOWS_SHELL_TIMEOUT_MS. false without a bus, for a NULL name and on errors. */
bool windows_shell_name_owned(GDBusConnection *bus, const char *name);
/** Fill out with a source that asks Torchlight's extension in the shell owning borrowed
 * shell (WINDOWS_SHELL_GNOME or WINDOWS_SHELL_CINNAMON; copied) on bus (NULL: the session
 * bus) for the shell's windows, most recently used first, leaving out the launcher's own,
 * and to activate one. Each call waits at most WINDOWS_SHELL_TIMEOUT_MS; list returns TL_IO
 * while the extension is not running and activate TL_STATE for a closed window. The source
 * holds a bus reference released by its destroy. Calls block the thread making them.
 * TL_INVALID for a NULL out or shell, TL_IO when there is no session bus, TL_NOMEM; out
 * zeroed on error. */
tl_status windows_shell_source(GDBusConnection *bus, const char *shell, tl_window_source *out);
/** Enable the extension once for this user, through GNOME Shell on bus (NULL: the session
 * bus): when the shell has it installed but not enabled and allows user extensions, ask it
 * to enable it; once enabled (by us or already), create borrowed marker and its folders.
 * Later calls see marker and do nothing, so a person's own later disable stands. Also does
 * nothing while the shell is not running or has not found the extension (GNOME Shell looks
 * for new extensions at login). Blocks for at most three WINDOWS_SHELL_TIMEOUT_MS waits. No
 * errors: a failure leaves the extension as it was. */
void windows_shell_enable_gnome_once(GDBusConnection *bus, const char *marker);
/** Enable the extension once for this user in Cinnamon, through borrowed settings (the
 * org.cinnamon schema, or any schema with an "enabled-extensions" string list): when the
 * extension is installed under one of borrowed data_dirs (a NULL-terminated list, as
 * cinnamon/extensions/UUID/metadata.json) and not yet listed, add it to the list; once listed
 * (by us or already), create borrowed marker and its folders. Later calls see marker and do
 * nothing, so a person's own later disable stands. Cinnamon loads an extension the moment it
 * is listed; the setting's backend may write it later (g_settings_sync waits for that). No
 * errors: a failure leaves the setting as it was. */
void windows_shell_enable_cinnamon_once(GSettings *settings, const char *const *data_dirs,
                                        const char *marker);
#endif
