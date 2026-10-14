# 0054. Open windows on Cinnamon's Wayland session, through a Cinnamon extension

- **Status:** Accepted
- **Date:** 2026-10-14

## Context

Linux Mint offers Cinnamon on Wayland as an experimental session. Its window manager,
muffin, offers neither of the task bar protocols of
[ADR 0052](0052-open-windows-through-wayland-taskbar-protocols.md). Cinnamon is a fork of
GNOME Shell, though, and runs extensions much like GNOME Shell's, so the GNOME approach
([ADR 0051](0051-open-windows-on-gnome-wayland.md)) carries over.

Testing the popup on a nested Cinnamon Wayland session turned up a second problem. The
popup survived its first showing, but the compositor disconnected it on the second:
`gtk_shell::get_gtk_surface already requested`. GTK 4 releases a hidden window's
`gtk_surface1` only from `gtk_shell1` version 4 on, and muffin offers version 3. Hiding
therefore leaves the old object on the surface, and asking for a new one when the window
shows again is a protocol error. The resident popup hides and shows the same window every
time.

## Decision

1. **A Cinnamon port of the extension**, `ui/cinnamon/torchlight@antash-mishra.github.io`.
   It exports the same `org.torchlight.Windows` interface, at the same path, on Cinnamon's
   `org.Cinnamon` name, so the GNOME source serves both shells: `ui/gtk/windows_gnome.c`
   became `ui/gtk/windows_shell.c`, which takes the shell's bus name. The port differs from
   GNOME's in these ways:
   - It uses Cinnamon's module style and `Cinnamon.WindowTracker`.
   - It finds windows through `global.get_window_actors()`, since muffin has no
     `list_all_windows`.
   - It switches with `Main.activateWindow(window, time, workspace)`.
   - Its `cinnamon-version` is `["6.0"]`, which Cinnamon reads as a minimum.
2. **Installed with the package, turned on once by the popup.** The `.deb` and `make
   install` put it under `share/cinnamon/extensions`. On Cinnamon's Wayland session the
   popup adds it to `org.cinnamon enabled-extensions` once it is installed in a data
   folder, and writes `~/.local/state/torchlight/cinnamon-extension-enabled`. Cinnamon
   watches that setting and loads the extension at once, so no new login is needed. With
   the marker present the popup never adds it again, so a person who turns it off keeps it
   off.
3. **A fresh surface for each showing where GTK cannot release the old one.**
   `wayland_global_version` (`ui/gtk/wayland_globals.c`) reads the compositor's
   `gtk_shell1` version once. When it is below 4, `popup_view_set_fresh_surfaces` makes
   each hide also unrealize the window, so the next show gets a new surface.

## Alternatives considered

- **Turning the extension on for everyone through a GSettings override:** it replaces the
  distribution's own default list, and a person's own choice would be lost on upgrade.
- **Asking the person to turn it on in Cinnamon's Extensions settings:** a step people miss.
- **Building a new window for every showing:** slower, and it loses the popup's state.
- **Unrealizing on every Wayland desktop:** it would slow GNOME's showings, whose newer
  `gtk_shell1` lets GTK release the object; other compositors have no `gtk_shell1`.
- **Leaving it:** each showing after the first would cost a new process, because the
  disconnected popup exits and the shortcut starts another.

## Consequences

- On Cinnamon's Wayland session (6.0 or newer) the popup lists windows and switches
  between them from the first time it opens after installing.
- Showing the popup again on Cinnamon's Wayland session creates a new surface and renderer
  each time; other desktops are unchanged.
- Tests:
  - **Unit:** `tests/unit/test_windows_shell.c` runs fake GNOME Shell and Cinnamon
    instances on a private bus. Turning the Cinnamon extension on is tested against
    settings held in memory: not when it is not installed, once, never after a person's own
    disable, and without a duplicate when it is already listed.
  - **End to end:** `tests/test_windows_desktops.py` runs Cinnamon 6.4 as a nested Wayland
    compositor on Xvfb, with the extension installed for the person but off. The popup
    turned it on, Cinnamon loaded it without a new login, the popup listed both windows
    most recent first and still worked after being shown again, and switching through the
    source moved the older window first.
