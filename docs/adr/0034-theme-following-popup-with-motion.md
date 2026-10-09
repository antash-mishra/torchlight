# 0034. Theme-following popup with bounded motion

- **Status:** Accepted
- **Date:** 2026-10-09

## Context

The Quiet System popup ([ADR 0027](0027-quiet-system-native-popup.md)) read as a
terminal rather than a desktop app: monospace everywhere, `//` and `/` prompts, an
underscore caret, fixed graphite and moss colors, and 10–11px lowercase key hints.
Its preferred faces, JetBrains Mono and Space Grotesk, were not installed on the
target Cinnamon machine, so GTK silently rendered DejaVu Sans Mono and Noto Sans.
Every file shared one symbolic icon. Two results with the same name could show
identical shortened paths, because shortening drops leading folders first: the
two libc++ `set` headers differ only by NDK version (`27.0.12077973` and
`27.1.12297006`).

The user compared three directions in a design study
([popup directions](../popup-directions.html)): blend in with the desktop theme,
keep the Quiet System look with better type, or a command palette. They chose the
first, in light and dark, with motion to keep a recognizable Torchlight identity.

## Decision

The popup follows the desktop:

- Colors come from GTK's named colors (`@theme_bg_color`, `@theme_fg_color`,
  `@theme_selected_bg_color`), so light and dark themes, the accent and
  high-contrast themes apply without code. Mint-Y defines no `@accent_bg_color`,
  so the selected-background color serves as the accent.
- No `font-family` is set; the desktop font applies. Text sizes are relative,
  and none is below 0.9em (12px at GTK's default 10pt).
- One search field holds a symbolic search icon, the plain GtkEntry with GTK's
  native caret, and inline no-match/offline feedback with Retry. The underscore
  caret overlay is removed. The inline text appears only when it has something to
  say, so the query never shifts while typing.
- Rows show full-color icons: application icons, `folder`, and file types guessed
  from the name alone with `g_content_type_guess` (no file access). Names the
  guess cannot place keep `text-x-generic`.
- Footer hints are keycaps with desktop wording ("Show in Folder"). Optional hints
  (Select, then Show in Folder) are hidden when the measured surface would exceed
  the popup width, so a large desktop font never widens the window.
- Rows of the same kind and name keep the folder where their parents first differ
  (`popup_model_distinct_prefix`); `path_label` keeps that head, then as many
  trailing folders as fit.

Motion uses GTK CSS keyframes and transitions, except the selection glide. Nothing
waits for an animation, nothing loops, and GTK's `gtk-enable-animations` setting
turns all of it off:

- **Open:** the surface fades in from 6px above (160ms). The *torch sweep*, a band
  of accent light, crosses the search field once per show (560ms after 60ms), by
  animating `background-position` of a gradient. An `opening` class is removed
  after 700ms so the next show restarts it.
- **Typing glow:** the existing `typing` class and 700ms hold, restyled with the
  accent.
- **Results:** the first results cascade in (140ms, 18ms apart, eight steps at
  most). Later renders fade in only ids that were not already on screen; the
  launcher remembers the shown ids.
- **Selection:** a new `selection_track` widget draws one highlight, carrying the
  Enter keycap, beneath the list box and eases it over 130ms using a frame-clock
  tick callback. Rows stay transparent. It jumps when animations are off, when
  results first appear and after relayout.
- **Searching:** a GtkStack crossfades the search icon into a spinner when a query
  is still pending after the existing 120ms timer.
- **Launch:** the selected row flashes and its icon pulses while resolve and launch
  run; a failure clears it.
- **Dismiss:** queries and actions are canceled immediately; only hiding the window
  waits for a 110ms fade. Showing again cancels a pending hide.

The empty popup is 70px high. The stylesheet is embedded compressed, because its
uncompressed literal exceeded ISO C's 4095-byte guarantee under `-Wpedantic`.

## Alternatives considered

Keeping the Quiet System look with installed fonts kept its identity but still
ignored light themes and accent changes. A command palette with a detail pane,
an action list and scope filters would be more capable, but needs asynchronous
file metadata, a kind filter in the query protocol and new action paths; it can
build on this change later. libadwaita's animation and style helpers would be a
new dependency, while GTK CSS animations plus one tick callback cover the need.
Animating the window height as results change would resize and redraw the X11
toplevel on every frame, so rows animate instead. Per-row CSS providers for
cascade delays use deprecated API; eight static classes suffice.

## Consequences

The popup looks native on any GTK4 theme that defines the standard named colors,
as GTK's default theme and Mint-Y do. Its typeface now depends on the desktop: on
the target machine the interface font is Noto Mono 13, so the popup renders in
monospace there. This supersedes the presentation in ADR 0027; its IPC, keyboard,
fixed-geometry and path-safety contracts remain.

Native tests replace the caret pixel checks with highlight geometry and glide,
icons, entrance classes, opening and closing timers, the spinner and the launch
flash. Model tests cover distinct prefixes. The isolated acceptance matrix
(light, dark at 150% fonts, high contrast at 2× scale, a reserved panel) and the
AT-SPI run pass with the new presentation; see `docs/ui/validation.json`.
