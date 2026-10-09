# 0035. Open windows under application results

- **Status:** Accepted; implemented (layout revised 2026-10-10)
- **Date:** 2026-10-09

## Context

Typing `chrome` finds Google Chrome, but Enter always launches it. When Chrome
already has windows open, the user usually wants one of those windows, and
sometimes a new one. The popup has no way to show or reach them.

The target session is Cinnamon on X11 (ADR 0014). The popup already links
libX11 for placement. A survey of a real session found:

- Window managers publish managed windows in `_NET_CLIENT_LIST_STACKING`,
  each with `WM_CLASS` (instance and class), `_NET_WM_NAME` and, for GTK
  applications, `_GTK_APPLICATION_ID`.
- Desktop entries declare `StartupWMClass` for the class their windows use:
  `google-chrome.desktop` declares `google-chrome`, which is the instance of
  both open Chrome windows.
- Process ids are unreliable: sandboxed Brave reports `_NET_WM_PID` 2.
- Chrome web apps (`crx_<id>` instances) have their own desktop entries, and
  their windows report class `Google-chrome`. VS Code declares
  `StartupWMClass=Code` but its windows report `code`.
- Reading the list and five properties per window costs about 90 µs per
  window over the local X connection (12 windows: 1.1 ms).

Wayland gives ordinary clients no portable way to list or activate other
applications' windows.

## Decision

1. **Scope.** Only application results get windows. Settings panels, files
   and folders do not: editors read and close files, so an open file cannot be
   detected, and title matching is ambiguous (`README.md`). Searching windows
   by title is a separate later step.
2. **Daemon.** Desktop entries keep `StartupWMClass`, and application results
   carry it as an optional `wm_class` field (absent when the entry has none or
   it exceeds 255 bytes). The field is additive, so `IPC_VERSION` stays 1.
   The daemon never sees windows: they belong to the display session, and the
   search path is unchanged.
3. **`windows` module.** A pure, unit-tested snapshot of open windows, read
   from a swappable `tl_window_source` (list, activate, destroy). The X11
   source lives with the popup and uses GTK's own display connection under
   GDK error traps. It lists `_NET_CLIENT_LIST_STACKING` from top to bottom,
   so the most recently used window comes first. It skips the popup, windows
   whose type is not normal, and skip-taskbar windows. Titles are converted to
   valid single-line UTF-8. Activation sends `_NET_ACTIVE_WINDOW` with
   source indication 2 (pager) and GDK's last user event time, so the window
   manager raises the window, restores it and switches workspace. A window
   that has left the client list reports `TL_STATE`. Without an X11 display
   there is no source, and the popup behaves exactly as before.
4. **When the snapshot is taken.** Once per popup show, in an idle callback
   after presenting, so it never delays the first paint, and again after
   activating a window that has closed. Typing never reads windows.
5. **Matching.** Each window goes to at most one displayed application row,
   the one with the strongest evidence; ties go to the higher-ranked row.
   Evidence, strongest first:
   1. `StartupWMClass` equals the window's instance;
   2. `StartupWMClass` equals the window's class;
   3. the GTK application id plus `.desktop` equals the desktop id;
   4. the instance, as is or lowercased with spaces as hyphens, plus
      `.desktop` equals the desktop id.

   Comparisons are exact, as in GNOME Shell and Cinnamon's window trackers.
   A case-insensitive class match would put Chrome web-app windows under
   Chrome. VS Code matches through rule 4.
6. **Hierarchy.** The popup model flattens results and children into one
   list of items: result, window, "Show N more windows" and "New window".
   Every application with windows lists them beneath it, wherever it ranks:
   up to three (most recent first), then a More item if there are others, then
   New window. The application row itself stays as it was: the listed windows
   show that it is running, so no count is drawn. Left on a child collapses
   its application and selects it; Right, with the caret at the end of the
   query, lists the windows again or reveals the rest from a More item.
   Expansion choices last for one request id. Retained selection across
   lexical/final phases and window refreshes is keyed by (result id, item
   kind, window handle).
7. **Actions.** Enter on an application row with windows brings forward its
   most recent window. Enter on a window brings that window forward; on New
   window it runs the entry's `new-window` (or `new-empty-window`) desktop
   action, or a normal launch when there is none; on More it shows every
   window. Ctrl+Enter on an application row keeps its existing behaviour. A
   window activation records the same asynchronous open as a launch, so
   personal ranking (ADR 0033) learns from it.
8. **Bounds.** 128 windows per snapshot, 255-byte class/instance/app id and
   511-byte titles. The model's item list has a fixed capacity covering every
   result, window and child row.

## Alternatives considered

- **Window list in the daemon.** It has no display connection, would need
  one per session, and window state is unrelated to `catalog_gen`.
- **Process ids or `/proc` scans.** PIDs are wrong for sandboxed browsers,
  and open file descriptors do not show files that editors have closed.
- **Reading desktop files in the popup.** It would duplicate the daemon's
  catalog and do file I/O on GTK's main thread.
- **Case-insensitive `StartupWMClass` matching.** It assigns Chrome web-app
  windows to Chrome.
- **A worker thread with its own X connection.** It avoids main-thread
  round trips, but Xlib error handlers are process-global and GDK's traps
  only cover GDK's own connection. The measured cost does not justify it.
- **xcb request pipelining.** It would add a direct dependency (x11-xcb) to
  save about a millisecond.
- **Expanding only the first result** (the first version of this ADR, with
  five windows and an "Application · N windows" subtitle elsewhere). In use,
  people could not tell that a lower-ranked application had windows, and had
  to type until it reached the top. Every application now lists its windows,
  capped at three. Short queries that match several running applications push
  files further down; that is accepted.
- **A window-count pill on each running application** ("2 windows" with an
  overlapping-windows icon). Tried on 2026-10-10 and removed at the user's
  request: with the windows listed beneath it, the count repeated what the
  list already shows.
- **A count pill with windows shown only once selected.** Compact, but still
  needs a key press to see anything, which was the complaint.
- **Tab to expand.** Tab already moves keyboard focus to Retry.

## Consequences

- Enter on a running application now switches to it instead of launching it
  again. New window keeps the old behaviour one row down.
- Applications without a `new-window` action decide for themselves what a
  repeated launch does, and some single-instance applications only focus
  their existing window.
- Windows that open or close while the popup is visible are not shown until
  the next show. A window that closed in the meantime reports "That window
  has closed" and refreshes the list.
- Applications that declare no `StartupWMClass` and whose instance does not
  match their desktop file name have no windows listed.
- Wayland sessions show no windows until a compositor-specific source is
  added behind the same interface.
