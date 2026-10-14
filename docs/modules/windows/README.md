# windows

> **Status:** Implemented (ADRs 0035, 0051, 0052, 0053 and 0054), verified on Xvfb with metacity, against the window properties of a real Cinnamon X11 session, and on Wayland: headless GNOME Shell 46, Sway 1.9, labwc 0.7.1, KWin 5.27 and 6.7.5, and nested COSMIC and Cinnamon 6.4
> **Source:** `ui/gtk/windows.c` (snapshot, matching, text copying), `ui/gtk/windows_session.c` (choosing a source), `ui/gtk/windows_x11.c` (X11), `ui/gtk/windows_wayland.c` with `ui/gtk/windows_track.c` (Wayland task bar protocols), `ui/gtk/windows_shell.c` (GNOME Shell and Cinnamon extensions), `ui/gtk/windows_kwin.c` (KWin scripts); the extensions in `ui/gnome-shell/` and `ui/cinnamon/`; protocol files in `third_party/wayland-protocols/`
> **Headers:** `include/torchlight/windows.h`; private `ui/gtk/windows_{session,x11,wayland,track,shell,kwin}.h`
> **Tests:** `tests/unit/test_windows.c`, `tests/unit/test_windows_track.c`, `tests/unit/test_windows_shell.c`, `tests/unit/test_windows_kwin.c`, hierarchy cases in `tests/unit/test_popup.c`, `check_open_windows` in `tests/test_popup.py`, `tests/test_windows_gnome_shell.py`, `tests/test_windows_desktops.py`

## Purpose

Lists the open top-level windows of the desktop session so the popup can show
them beneath their application's result, and brings a chosen window forward.
Window state belongs to the display session, so this module lives with the
popup and never with the daemon.

## Responsibilities

- Owns a bounded snapshot of open windows, most recently used first.
- Decides how strongly a window belongs to an application (`windows_evidence`).
- Shortens window titles that end with their application's name.
- Talks to the window system only through a swappable `tl_window_source`.
- Does **not** assign windows to result rows or build the list: `popup_model`
  does, from this module's evidence. It does not decide when to take a
  snapshot: the launcher does, once per popup show.

## Public API

| Function | Description |
|---|---|
| `windows_create(source, &out)` | Empty snapshot over a copied source; owns its context on success. NULL source: never any windows. |
| `windows_destroy(windows)` | Destroys the snapshot and its source. |
| `windows_refresh(windows)` | Replaces the snapshot with the source's current windows; empty on error. |
| `windows_count` / `windows_get` | Read the snapshot; borrowed windows expire on refresh. |
| `windows_activate(windows, index)` | Ask the window manager to show and focus a window; `TL_STATE` if it has closed. |
| `windows_evidence(window, desktop_id, wm_class)` | How strongly a window belongs to an application. |
| `windows_short_title(title, app_name, out, size)` | Title without a trailing " - Application" suffix. |
| `windows_copy_name(out, size, name)` / `windows_copy_text(out, size, bytes, length, truncated)` | For sources: exact identifiers (empty when too long) and titles as clean single-line UTF-8. |
| `windows_session_source(popup, state_dir, &source)` | Private: the source for the session the popup runs in, turning a shell extension on once where needed; `TL_STATE` when the session has none. |
| `windows_x11_source(popup, &source)` | Private: the X11 source for the popup's display; `TL_STATE` when not X11. |
| `windows_wayland_source(display, &source)` | Private: the wlroots or COSMIC task bar protocol source; `TL_STATE` when the compositor offers neither. |
| `windows_track_*` | Private: windows reported one event at a time, listed most recently used first. |
| `windows_shell_source(bus, shell, &source)` | Private: the source over Torchlight's GNOME Shell or Cinnamon extension (`WINDOWS_SHELL_GNOME`, `WINDOWS_SHELL_CINNAMON`); `TL_IO` from `list` while it is not running. |
| `windows_shell_enable_gnome_once(bus, marker)` / `windows_shell_enable_cinnamon_once(settings, data_dirs, marker)` | Private: turn the extension on for the person once, then leave their choice alone. |
| `windows_shell_desktop(desktops, name)` / `windows_shell_name_owned(bus, name)` | Private: whether `XDG_CURRENT_DESKTOP` names a desktop, and whether a bus name has an owner. |
| `windows_kwin_source(address, folder, &source)` | Private: the KWin script source; `TL_IO` from `list` when KWin does not answer. |

## Design

**Evidence.** Ranked strongest first, mirroring the window trackers of GNOME
Shell and Cinnamon:

1. `StartupWMClass` equals the window's WM_CLASS instance;
2. `StartupWMClass` equals the WM_CLASS class;
3. the GTK application id plus `.desktop` equals the desktop id;
4. the instance, as is or lowercased with spaces as hyphens, plus `.desktop`
   equals the desktop id.

Comparisons are exact. Chrome web apps report instance `crx_<id>` and class
`Google-chrome`, while Chrome declares `StartupWMClass=google-chrome`, so a
case-insensitive class match would list web-app windows under Chrome. VS Code
declares `Code` but reports `code` and matches through rule 4. Process ids are
not used: sandboxed browsers report PID 2. The one exception is a window whose
source set `fold_case`: KDE Plasma 5.27 lower-cases X11 class names, so for its
windows rules 1 and 2 ignore ASCII case.

**Choosing a source** ([ADR 0052](../../adr/0052-open-windows-through-wayland-taskbar-protocols.md)).
`windows_session_source` tries, in order: X11 when the display is X11; the Wayland task
bar protocols when the compositor offers them; then the desktop shell whose window manager
owns its bus name (`org.gnome.Shell`, `org.Cinnamon`, `org.kde.KWin`); then the one
`XDG_CURRENT_DESKTOP` names, whose shell may still be starting. Bus names decide first
because LXQt, Xfce and others run on several compositors. For GNOME and Cinnamon it also
turns the extension on once. The launcher calls it once, when it builds its window.

**X11 source.** Reads `_NET_CLIENT_LIST_STACKING` (or `_NET_CLIENT_LIST` when
the window manager publishes no stacking order) from top to bottom, skipping
the popup itself, windows whose first `_NET_WM_WINDOW_TYPE` is not normal,
untyped transient windows (dialogs) and `_NET_WM_STATE_SKIP_TASKBAR` windows.
Titles come from `_NET_WM_NAME`, else a Latin-1 `WM_NAME`, and are made valid
single-line UTF-8 with controls as spaces. A value cut at the read limit drops
its last character rather than ending inside one. Activation first checks
that the window is still in the client list, then sends `_NET_ACTIVE_WINDOW`
with source indication 2 (pager) and GDK's last user event time, so the window
manager raises and restores the window and switches workspace. Everything runs
on GTK's own connection under GDK error traps, so a window destroyed mid-read
only drops that window.

**Wayland task bar protocols** ([ADR 0052](../../adr/0052-open-windows-through-wayland-taskbar-protocols.md)).
Sway, Hyprland, labwc (Budgie, LXQt, Xfce), Wayfire and niri offer
`zwlr_foreign_toplevel_manager_v1`; COSMIC offers the standard
`ext_foreign_toplevel_list_v1` with its own `zcosmic_toplevel_info_v1` (state) and
`zcosmic_toplevel_manager_v1` (activation). `windows_wayland_source` uses GTK's own
connection. It reads the current windows with a few round trips on a private queue, then
moves every object to GTK's default queue, whose dispatch keeps the windows current, so
`list` and `activate` never wait. These protocols keep no history, so `windows_track`
stamps each window when it becomes active; windows not seen active keep the compositor's
order, which on wlroots is newest first. The app id also fills class and instance, since an
Xwayland window reports its WM_CLASS class there. COSMIC's manager is never sent `destroy`:
cosmic-comp panics on it, taking the session down.

**KWin scripts** ([ADR 0053](../../adr/0053-open-windows-through-kwin-scripts.md)).
Each list or activation writes a short ES5 script to `$XDG_RUNTIME_DIR`, has KWin load and
run it (`/Scripting/Script<id>` on Plasma 6, `/<id>` on 5.27), and unloads it. The script
reports with `callDBus` to `/org/torchlight/KWin` on the source's own connection, whose
private main context only these calls run. Only KWin's unique name and the call's token
count, and the whole call waits at most 250ms. Windows come topmost first by
`stackingOrder`; `desktopFileName` gives the application id. A handle is the first 64 bits
of KWin's UUID, and an id goes into a script only when it holds hex digits, braces and
dashes alone.

**GNOME Shell and Cinnamon extensions** ([ADR 0051](../../adr/0051-open-windows-on-gnome-wayland.md),
[ADR 0054](../../adr/0054-open-windows-on-cinnamon-wayland.md)).
On GNOME's Wayland session, applications cannot see other applications' windows, so
Torchlight's GNOME Shell extension does it for the popup. It exports
`org.torchlight.Windows` at `/org/torchlight/Windows` on the shell's `org.gnome.Shell`
name: `List` returns the shell's tab list (normal windows on every workspace, most
recently used first) as (id, instance, class or Wayland application id, application id,
title), and `Activate(id)` switches to the window as the overview does. The application
id is the desktop file the shell matched the window to, without `.desktop`, so the
application-id rule below matches it exactly; windows the shell could not match fall
back to their GTK application id. `windows_shell_source` calls these with GIO, waiting
at most 250ms, never auto-starting a service, and leaves out the popup's own window
(`org.torchlight.Launcher`). Cinnamon's port serves the same interface on `org.Cinnamon`;
it finds windows through their actors, since Cinnamon's window manager lacks
`list_all_windows`. GNOME's extension is turned on through `org.gnome.Shell.Extensions`,
once GNOME has found it at login; Cinnamon's by adding it to `org.cinnamon
enabled-extensions`, which Cinnamon applies at once.

**Titles.** The suffix after the last " - ", " – " or " — " is removed when it
names the application, ignoring ASCII case: the whole name ("Google Chrome"),
its first or last words ("Brave" for "Brave Web Browser", "Chrome") or text
ending with its first word ("Mozilla Firefox" for "Firefox Web Browser").

## Data flow

```
popup shown ─► idle ─► windows_refresh ─► X11 source (EWMH properties)
                              │            or Wayland source (windows kept from protocol events)
                              │            or shell source ─► extension's List (D-Bus)
                              │            or KWin source ─► script ─► Report (D-Bus)
                              │
                              ▼
          popup_model_set_windows ─► windows_evidence per (window, application row)
                              │
                              ▼
   items: every application, up to 3 windows, more windows, new window ─► view rows
Enter on a window ─► windows_activate ─► _NET_ACTIVE_WINDOW, activate(seat), Activate(id)
                                         or a script ─► history open (IPC)
```

## Invariants

- A snapshot holds at most `WINDOWS_MAX` (128) windows, none with handle zero.
- Instance, class and application id fit in 255 bytes or are left empty;
  titles fit in 511 bytes, end on a whole UTF-8 character and contain no
  control characters.
- Indices and borrowed windows are valid until the next refresh or destroy;
  `popup_model_set_windows` must follow every refresh.

## Performance

Measured on 2026-10-09 in a Cinnamon X11 session: listing 12 windows with
their type, state, class, application id and title took 1.05 to 1.7 ms (about
90 µs, or five round trips, per window). The snapshot is taken in an idle
callback after the popup is presented, so it never delays the first paint, and
typing never reads windows. The Wayland task bar source only copies the windows it
keeps. A KWin list is a script round trip: 15ms measured on Plasma 5.27, including
starting three `gdbus` processes. Matching costs one evidence check per window and
application row (at most 128 × 11). The daemon's query path is unchanged.

## Testing

`tests/unit/test_windows.c` uses a fake source to cover the unsupported source,
dropped zero handles, the 128-window bound, list failures, activation of
closed windows, the evidence ranks with WM_CLASS values from a real session
(Chrome, a Chrome web app, VS Code, GNOME Terminal, Zed) and title
shortening, including UTF-8 truncation. `tests/unit/test_popup.c` covers
assignment and the item hierarchy, including applications below the top
result. `tests/test_popup.py` maps real windows
with `tests/fixtures/window_app.c` on an isolated X server and checks Enter,
window choice, Left/Right, a window that closed after the snapshot, an
application ranked below an exact file name and new window.
`tests/unit/test_windows_shell.c` runs the shell source against fake GNOME Shell and
Cinnamon instances on a private bus: order, the popup's own window and a zero handle left
out, cleaned titles, too-long identifiers dropped, activation of open and closed windows,
an extension that is not running, which shell owns its name, and enabling the extension
once in each (not when GNOME did not find it or extensions are off, not when Cinnamon's is
not installed, never again after a person's own disable, no duplicate in Cinnamon's
list). `tests/unit/test_windows_track.c` covers the Wayland source's ordering, readiness,
names and removal. `tests/unit/test_windows_kwin.c` runs the KWin source against a fake
KWin: both script paths, a script name left loaded, unsafe ids and short entries left out,
activation, a silent KWin, a forged report, nonsense, and every script unloaded. `tests/test_windows_gnome_shell.py` runs
a headless GNOME Shell with the extension installed but disabled, the fixture daemon and
the real popup: the popup enables the extension, which lists two test windows most
recently used first and switches between them, and the popup lists both under their
application. `tests/test_windows_desktops.py` does the same on headless Sway and labwc,
KWin (5.27, or 6 from KDE neon's packages) and nested COSMIC and Cinnamon: the popup lists
two test windows under their application, most recent first; the session's source, through
`tests/fixtures/window_sources.c`, switches to the older one; and the popup then lists it
first. On Cinnamon the popup also turns the extension on. Run the unit tests with `make
test`, the X11 checks with `make test-ui-isolated`, the GNOME check with `make
test-ui-gnome` and the other desktops with `make test-ui-desktops`. They skip what is not
installed; `TORCHLIGHT_GNOME_ROOT`, `TORCHLIGHT_WLROOTS_ROOT`, `TORCHLIGHT_COSMIC_ROOT` and
`TORCHLIGHT_KWIN_ROOT` point them at unpacked desktops.

## Gotchas

- Wayland compositors with neither a task bar protocol nor a supported shell (plain Weston,
  kiosk compositors) list no windows. On GNOME the extension works from the person's next
  login after installing, since GNOME looks for new extensions at login.
- On the Wayland task bar protocols, the order of use starts when the popup starts; until
  then a window keeps the compositor's place. Hyprland, niri and Wayfire were not run end
  to end; they speak the same protocol as Sway and labwc.
- A KWin script call blocks the popup's main thread, after the first paint, for about
  10ms, or 250ms when KWin does not answer.
- Applications that declare no `StartupWMClass` and whose instance does not
  match their desktop file name get no windows.
- Windows that open or close while the popup is visible appear at the next
  show; activating one that closed reports it and refreshes the snapshot.

## Related

- Modules: [ui](../ui/README.md), [desktop](../desktop/README.md), [ipc](../ipc/README.md)
- ADRs: [0035](../../adr/0035-open-windows-under-applications.md), [0051](../../adr/0051-open-windows-on-gnome-wayland.md), [0052](../../adr/0052-open-windows-through-wayland-taskbar-protocols.md), [0053](../../adr/0053-open-windows-through-kwin-scripts.md), [0054](../../adr/0054-open-windows-on-cinnamon-wayland.md)
- Plan: [Open windows on every Wayland desktop](../../wayland-windows-plan.md)
