# ui

> **Status:** M3 design specified; GTK implementation planned
> **Source:** `ui/tui/, ui/gtk/` · **Header:** `—`
> **Tests:** M3 acceptance checklist in `docs/m3-gui-design.md`; no UI tests yet

## Purpose
CLI-backed development followed by a GTK4 popup in M3. ncurses TUI is optional.

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
Follow the [GUI specification](../../m3-gui-design.md) and
[interactive preview](../../m3-gui-preview.html), recorded in
[ADR 0014](../../adr/0014-m3-gtk-popup-design.md). It defines a compact,
680-logical-pixel native-theme GTK4 popup, eight visible basename/parent rows,
status/error states, keyboard behavior, accessibility and Cinnamon X11 checks.

- Talks to the daemon only via `ipc`.
- Desktop shortcut invokes/toggles the popup; verify focus on target X11/Wayland.
- Arrow keys select; Enter opens; Escape dismisses.
- Ctrl+Enter reveals through file-manager D-Bus, with parent-directory fallback.
- Invoke open commands through argv rather than shell interpolation.
- Request ids suppress stale responses; show indexing status.
- Render the `lexical` phase immediately, then replace in place with the `final`
  phase. Selection is by file id; keep a user-selected row until they change
  selection or query, even if it drops out of final top-k.
- Resolve file ids against the current catalog before launch; handle stale
  results. Open/reveal exact `path` bytes or decoded `path_b64`, never `display`.
- Reports accepted launches, not guaranteed external application success.

## Data flow
_TODO after implementation._

## Invariants
_TODO after implementation._

## Performance
_TODO: complexity, memory use, `make bench` numbers with date._

## Testing
_TODO after implementation._

## Gotchas
_TODO after implementation._

## Related
- [ipc](../ipc/README.md)
