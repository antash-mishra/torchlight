# 0051. Open windows on GNOME's Wayland session, through a GNOME Shell extension

- **Status:** Accepted
- **Date:** 2026-10-14

## Context

The popup lists a running application's open windows beneath it and switches to one
([ADR 0035](0035-open-windows-under-applications.md)). That came from X11, where any
client can read the window manager's list. Wayland gives applications no way to see or
raise other applications' windows, so on Wayland the list was empty
([ADR 0049](0049-popup-on-wayland.md)) and Enter on a running application only started it
again. The user asked for the list on Wayland too and chose GNOME, Ubuntu's default
desktop, first.

GNOME Shell itself knows every window, its application (it matches windows to desktop
files) and the order they were last used, and it can switch to any of them. Its own
D-Bus interfaces do not offer that to other programs: `org.gnome.Shell.Eval` and the
introspection interface are restricted to GNOME's own tools. Code that runs inside the
shell, an extension, can.

## Decision

1. **A small GNOME Shell extension**, `ui/gnome-shell/torchlight@antash-mishra.github.io`
   (GNOME 46 to 49). Enabled, it exports `org.torchlight.Windows` at
   `/org/torchlight/Windows` on the shell's `org.gnome.Shell` bus name, with two methods:
   `List`, which returns the normal windows of every workspace, most recently used first,
   as (id, WM_CLASS instance, class or Wayland application id, application id, title);
   and `Activate(id)`, which switches to the window's workspace and focuses it, as the
   overview does, and says whether it still existed. The application id is the desktop
   file GNOME Shell matched the window to, without `.desktop`, or else the GTK
   application id, so `windows_evidence` matches it exactly.
2. **A GNOME source behind the existing interface.** `ui/gtk/windows_gnome.c` implements
   `tl_window_source` over that interface with GIO, waiting at most 250ms per call, and
   leaves out the launcher's own popup. The launcher uses it when X11 is not available and
   `XDG_CURRENT_DESKTOP` names GNOME; snapshots, matching, the list and window switching
   are unchanged.
3. **Installed with the package, enabled once by the popup.** The .deb (and `make
   install`) puts the extension under `share/gnome-shell/extensions`. GNOME looks for new
   extensions at login, and each person enables extensions for themselves, so the first
   time the popup starts after that, it asks GNOME Shell (`org.gnome.Shell.Extensions` at
   `/org/gnome/Shell`) to enable it, unless the person has switched extensions off, and
   writes `~/.local/state/torchlight/gnome-extension-enabled`. With that marker present it
   never asks again, so a person who disables the extension keeps it disabled.

## Alternatives considered

- **`org.gnome.Shell.Eval`**: disabled outside GNOME's unsafe mode since GNOME 41.
- **The XWayland window list**: sees only the X11 applications running under XWayland,
  not native Wayland ones.
- **Enabling the extension for everyone from the package** through a GSettings override:
  overrides of `enabled-extensions` replace the distribution's own default list (Ubuntu
  enables its dock that way), and a person's own choice would be lost on upgrade.
- **Other desktops now**: KDE (scripting through KWin's D-Bus interface), Sway and
  Hyprland (their control sockets) and Cinnamon's Wayland session (its own extension)
  each need their own source; GNOME came first.

## Consequences

- On GNOME's Wayland session the popup lists windows and switches between them as on
  X11, once the person has logged in again after installing (GNOME finds new extensions
  at login) and opened the popup once.
- Any program the person runs outside a sandbox can call the interface, which reveals
  window titles and can switch windows, as X11 always allowed. Sandboxed (Flatpak)
  applications cannot reach the shell's bus name.
- Each GNOME release may need the extension's `shell-version` list, and possibly its code,
  updated; an extension whose list does not name the running GNOME is shown as out of
  date and not run, which leaves the popup without windows but otherwise working.
- Tested against a fake shell on a private bus (`tests/unit/test_windows_gnome.c`) and
  end to end on a headless GNOME Shell 46 (`tests/test_windows_gnome_shell.py`, `make
  test-ui-gnome`): the popup enabled the extension once, the extension listed two windows
  most recently used first and switched between them, and the popup listed both under
  their application. The test skips where GNOME Shell is not installed.
