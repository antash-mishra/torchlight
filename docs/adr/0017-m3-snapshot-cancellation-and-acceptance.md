# 0017: M3 consistent metadata, canceled actions and repeatable GUI acceptance

**Status:** Accepted, implemented

## Context

The M3 review found that desktop discovery read metadata and its revision from
different file contents during atomic replacement. The resulting old label
could carry the replacement's revision and remain unchanged on later refreshes.
A reveal worker could also start its parent fallback after Escape, and valid
Settings category lists without their optional final semicolon were misclassified.

Expanded acceptance found that the window's keyboard handler intercepted Enter
on Retry, the actual editable accessibility node had no name, and an initially
oversized scale-2 window could be incorrectly centered by the window manager.
An accepted worker finishing after Escape could dismiss a reopened search.

## Decision

Read each desktop keyfile once for discovery. Construct GDesktopAppInfo and
calculate the content revision from that same parsed object. Keep filename-based
native construction and execution-key validation at launch, preserving desktop
activation semantics and `%k`. Use GIO's category-list parser.

Carry the existing GCancellable through session-bus acquisition, ShowItems and
fallback spawning. Check cancellation immediately before spawning. Accepted
requests retain their history result, but canceled completion cannot change or
close a later popup search. Allow GTK to handle keys when Retry has focus.
Name both the search entry and its delegated editable accessibility node.

Clamp X11 popup dimensions before mapping, then request placement on map. Choose
the monitor by its full geometry and place within its work area, including when
the pointer is on a panel outside that area.

Run GUI automation in its own display and session bus with fixture applications.
Expose only the accessibility bus when requested: unrelated desktop services can
block private-session GTK startup and make a harness timeout inconclusive.
Use test-only dynamic observers for deterministic file replacement, delayed
accepted opening and GTK after-paint timestamps. No test hook or extra I/O is
compiled into the production query or popup paths.

## Consequences

Atomic replacements receive new result ids on refresh; old revisions cannot
describe newly read metadata. Escape suppresses subsequent launch work without
erasing already accepted history. Error recovery works with the keyboard and
search controls have names exported through AT-SPI.

The repeatable GUI tests require optional existing system tools: Xvfb, an X11
window manager, xdotool and D-Bus. Accessibility checks additionally use Python
GI/AT-SPI. They add no runtime dependency to Torchlight. Native after-paint
measurements include debounce, IPC and GTK rendering, but do not measure hardware
display presentation. Physical fractional/multi-monitor configurations and a
human screen-reader session still need separate desktop validation.
