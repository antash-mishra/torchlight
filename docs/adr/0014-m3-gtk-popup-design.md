# 0014: M3 GTK popup presentation and interaction

**Status:** Accepted, implemented in M3; extended by ADR 0015

## Context

The plan defines keyboard actions and safe daemon communication, but the UI
module has no implemented popup or visual source. M3 needs a concrete layout,
state behavior and focus/selection contract before development. The target
desktop is Cinnamon X11; M2 provides lexical-only terminal responses.

## Decision

Follow [the GUI specification](../m3-gui-design.md) and
[interactive layout preview](../m3-gui-preview.html): a 680-logical-pixel floating
search window, a prominent entry, eight visible basename/parent rows, native
GTK theme/icons and a restrained status/keyboard footer. Use a single application
instance invoked by a desktop shortcut, with responsive asynchronous IPC.

Selection is by file id, retained after deliberate movement even if a later
phase drops it from top-k. Resolve the current id before opening/revealing exact
bytes; record accepted launches asynchronously. Surface indexing, unavailable
service, no matches and stale-action errors without stealing entry focus.

M1/M2 work specifies this design. GTK code, dependency setup, desktop integration,
focus validation and the systemd user unit belong to M3.

## Consequences

The implementation has an explicit visual and interaction acceptance checklist.
It can use the desktop's accessibility, scaling and theme behavior. Future
two-phase results fit the existing selection model. Placement and activation
must be verified in the target window system; the preview is a design artifact
and does not establish those behaviors.
