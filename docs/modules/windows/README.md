# windows

> **Status:** Implemented (ADR 0035), verified on Xvfb with metacity and against the window properties of a real Cinnamon X11 session
> **Source:** `ui/gtk/windows.c` (snapshot and matching), `ui/gtk/windows_x11.c` (X11 source)
> **Headers:** `include/torchlight/windows.h`, `ui/gtk/windows_x11.h` (private)
> **Tests:** `tests/unit/test_windows.c`, hierarchy cases in `tests/unit/test_popup.c`, `check_open_windows` in `tests/test_popup.py`

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
| `windows_x11_source(popup, &source)` | Private: the X11 source for the popup's display; `TL_STATE` when not X11. |

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
not used: sandboxed browsers report PID 2.

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

**Titles.** The suffix after the last " - ", " – " or " — " is removed when it
names the application, ignoring ASCII case: the whole name ("Google Chrome"),
its first or last words ("Brave" for "Brave Web Browser", "Chrome") or text
ending with its first word ("Mozilla Firefox" for "Firefox Web Browser").

## Data flow

```
popup shown ─► idle ─► windows_refresh ─► X11 source (EWMH properties)
                              │
                              ▼
          popup_model_set_windows ─► windows_evidence per (window, application row)
                              │
                              ▼
   items: every application, up to 3 windows, more windows, new window ─► view rows
Enter on a window ─► windows_activate ─► _NET_ACTIVE_WINDOW ─► history open (IPC)
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
typing never reads windows. Matching costs one evidence check per window and
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
application ranked below an exact file name and new window. Run the unit tests with `make test`, and the X11 checks with
`make test-ui-isolated`.

## Gotchas

- Wayland has no source, so no windows are listed there.
- Applications that declare no `StartupWMClass` and whose instance does not
  match their desktop file name get no windows.
- Windows that open or close while the popup is visible appear at the next
  show; activating one that closed reports it and refreshes the snapshot.

## Related

- Modules: [ui](../ui/README.md), [desktop](../desktop/README.md), [ipc](../ipc/README.md)
- ADRs: [0035](../../adr/0035-open-windows-under-applications.md)
