# 0065. The first edit after a pause searches at once

- **Status:** Accepted
- **Date:** 2026-10-24

## Context

The launcher coalesced query edits for 16 ms ([ADR 0015](0015-m3-desktop-catalog-and-launcher.md)):
every edit restarted a timer, and the query went to the daemon only once the timer ran
out. At most one query is out at a time, and the newest edit waits for its reply.

The window protected the daemon when a search took several milliseconds. A search now
takes under a millisecond (0.5 ms on the user's machine, request and reply included), so
for an ordinary key press, which comes 70 ms or more after the last one, the 16 ms were
pure waiting: the largest part of the time from a key to its results, ahead of drawing
them. The wait is in the launcher, so every desktop paid it, on X11 and on Wayland.

## Decision

1. **An edit after a pause searches at once.** When no window is open and no query is
   out, the edit's query goes to the daemon immediately. The edit also opens the 16 ms
   window.
2. **Edits within the window are coalesced as before.** An edit that comes while the
   window is open, or while a query is out, restarts the window, and its query goes when
   the window closes. A burst of keys or a paste therefore still sends the first edit and
   then one query for the end of the burst.
3. **A reply sends the newest edit only after the window.** While the window is open, a
   reply leaves the newest edit to the window's timer. Without this rule, with the first
   edit sent at once, each reply arriving within a burst sent the edit made meanwhile: on
   the user's GNOME desktop, where GTK hands over the keys of each frame together, nine
   keys typed 5 ms apart sent eight queries.

## Alternatives considered

- **A shorter window for every edit:** still a wait on every key, only smaller.
- **No window, only the one-query limit:** the end of a burst searches sooner, but every
  edit that lands between two replies is searched, a query per key in the bursts that
  GTK delivers frame by frame.

## Consequences

- On the user's GNOME X11 desktop, the median time from a key to its results on screen
  went from 44 ms to 28 ms when typing at 150 ms a key, and from 46 ms to 26 ms at 70 ms
  a key (interleaved runs of both versions). The rest is drawing the rows and waiting for
  a frame, which that desktop's compositor confirms to GTK 4 every 33 ms.
- The last key of a burst still waits the 16 ms window, as before.
- The X11 acceptance test checks that a key after a pause sends its query within 12 ms of
  the edit, and that the query for a burst typed at once leaves at least 15 ms after its
  last key. The popup probe notes when a query request is written to the daemon.
