# 0042. Desktop catalog: parse only when a change stamp moves

- **Status:** Accepted
- **Date:** 2026-10-11
- **Refines:** 0015 (M3 desktop catalog: "a worker rescans every second")

## Context
ADR 0015 kept application results fresh by re-reading and re-parsing every
`.desktop` file once a second. A real-desktop audit of M7 found this was the
daemon's largest idle cost: on the user's Cinnamon desktop, 171 entries and
about 1 MB of localized keys cost 1.39% of a core around the clock (87% of the
daemon's 1.59% idle total over seven minutes) and about 1.6 MiB/s of reads.
M7's idle gate is 0.2% of a core. The M7 scenario benchmark had not seen it
because its daemons ran with an empty `XDG_DATA_DIRS`.

Freshness still matters: an installed application should be searchable
within about a second, and an edited override should apply as quickly.

## Decision
- **Change stamp per check.** Every `check_ms` (default 1 s) the worker walks
  the applications trees with the same walker discovery uses and folds into a
  64-bit stamp, per `.desktop` entry, its path and the `stat` dev, inode, mode,
  size, mtime and ctime (following symlinks, as discovery does), plus a marker
  for every directory it cannot list. Per-item hashes are summed, so readdir
  order is irrelevant. It reads no file. A parse runs only when the stamp
  differs from the recorded one.
- **Stamp what the parse read.** Discovery computes the stamp of the files and
  directories it actually visited, and that stamp is recorded. A change the
  parse half saw, even one reverted before the next check (a directory that
  was unreadable only while the parse walked it), then differs from the next
  check. A failed parse records the check's stamp instead, so a tree that
  cannot be parsed is not retried every second.
- **Two timers for the one unstamped input.** GIO hides an entry whose
  `TryExec` binary is not on `PATH`, and that binary may appear after its
  `.desktop` file. The worker parses once more `settle_ms` (5 s) after the last
  stamp change and at least every `rescan_ms` (10 min).
- **Explicit refresh unchanged.** `desktop_refresh` (from `reconcile`) still
  forces a parse.
- `desktop_create` takes `tl_desktop_options` (NULL for defaults) so tests can
  use millisecond timing; `desktop_parses` counts completed discoveries. The
  wait uses a `CLOCK_MONOTONIC` condition variable.

## Alternatives considered
- **inotify on the applications directories.** Zero idle work, but
  `src/index` may not depend on `src/fs`, missing directories (a user
  `applications/` created later, Flatpak export trees) need parent watches or
  polling anyway, `IN_CLOSE_WRITE` would be needed for in-place edits, and the
  watches would share the per-user limit with the file indexer's 34k.
- **Directory mtimes only.** Cheaper, but misses in-place edits of an entry
  and fires on unrelated writes such as `mimeinfo.cache`.
- **A slower fixed poll (every 10 s).** Still parses 1 MB for nothing, just
  less often, and makes installs visible 10 s later.
- **Recording the stamp of the check that started a parse.** Simpler, but a
  directory unreadable only while a parse ran leaves its applications hidden
  until the next change or the 10-minute rescan. The regression test fails
  this way in 6 of 8 runs.

## Consequences
- An idle catalog costs one walk and about one `stat` per entry each second.
  Measured costs are in the [M7 plan](../m7-plan.md#real-desktop-review-2026-10-11).
- Install, removal and edit latency stay at most one second plus parse time;
  an entry hidden only by a missing `TryExec` binary appears within 5 s of
  the last change, or 10 minutes when the binary arrives on its own.
- The stamp is a 64-bit hash: a collision would delay one refresh until the
  next change or the 10-minute rescan.
