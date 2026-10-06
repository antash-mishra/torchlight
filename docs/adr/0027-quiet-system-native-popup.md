# 0027. Quiet System native popup

- **Status:** Accepted
- **Date:** 2026-10-07

## Context

The user selected a minimal developer-oriented launcher direction: a quiet
wordmark, 24px monospace input, an underscore cursor, useful shortened paths and
a stationary search field. Empty search must contain nothing below that field.
They requested subtle typing feedback and removed the clear icon. Shader art
was explored in a separate design study; the user explicitly excluded shaders
from this implementation.

## Decision

Implement the presentation with GTK4 widgets, embedded CSS and a small Cairo
caret overlay. An opaque `tl_popup_view` owns styling, widgets and presentation
timers. The launcher retains IPC, query state and native actions. A plain
GtkEntry replaces GtkSearchEntry, keeping native editing, clipboard, undo,
selection and accessibility while removing its built-in icons.

Use a graphite surface with a six-pixel radius, moss accent and quiet wordmark.
JetBrains Mono is preferred for the 24px query and file metadata, Space Grotesk
for application names; installed system monospace/sans fonts are fallbacks.
No new library dependency or global font installation is required.

The underscore follows GtkText's shaped cursor extents, including scrolling and
Unicode. Hide it during selection and focus loss; native caret rendering resumes
for input-method preedit. Honor GTK's cursor preferences and disabled animations.
Editing lights the search edge for 700ms after the latest change, with 120ms in
and 420ms out. Keep one hold timer; cancel it on focus loss, dismissal and teardown.

Collapse result viewport and footer when there are no matches. Show no-match,
offline and protocol-bound feedback inside the existing search area, with
keyboard-accessible Retry. Keep the X11 popup's top edge fixed as results change.
Measure the actual chrome before budgeting scroll height, so smaller/scaled
work areas remain usable. Shorten safe display paths using Pango measurements,
keeping home/root and trailing folders, then the middle of a very long final
folder. Preserve the full display string in tooltips and accessibility labels;
never derive action targets from shortened text.

## Alternatives considered

Keeping GtkSearchEntry preserves its built-in icons, conflicting with the
chosen minimal layout. Reimplementing text editing would put input methods and
accessibility at risk. Shader artwork is deferred by explicit user instruction.
Private bundled-font loading would require a direct Fontconfig dependency on
the current Pango baseline; installed-font fallbacks avoid that addition.

## Consequences

The presentation supersedes the theme-only geometry in ADR 0014; its input,
IPC and launch-safety contracts remain. CSS is embedded in the executable and
works outside the repository. There are no shader or image assets in the native
build. Exact typefaces depend on installed fonts. Wayland focus/placement remains
compositor-controlled.

Native tests check Cairo caret pixels, selection/IME fallback, large pasted
queries, Unicode path fitting, timer cleanup, collapse and fixed geometry.
X11 acceptance covers keyboard actions, restart/Retry, accessibility, high
contrast, scaling and bounded viewport height.
