# 0038. Repair scans only where inotify can miss changes; metadata events do nothing

- **Status:** Accepted
- **Date:** 2026-10-10
- **Replaces:** ADR 0011's "periodic scans (default 30 seconds)" rule
- **Refines:** 0012 (watch fallback) and 0030 (scoped reconciliation)

## Context
The [M7 plan](../m7-plan.md) profiled the daemon headlessly. inotify already
reported every create, delete and move, and scoped rescans made those visible
in about 0.1 s. Two things still cost CPU while nothing changed:

- **The 30-second full scan.** Every `rescan_ms` the writer walked every root,
  statted each entry twice, ran three SQL statements per row and rolled the
  transaction back: about 4 CPU-s and 100 MiB of SQLite temp-file writes per
  scan at 213k entries, 7 to 9% of a core around the clock.
- **Writes rescanned their folder.** `IN_CLOSE_WRITE` and `IN_ATTRIB` on a
  file scheduled a children rescan of its folder, whose refreshed mtime/size
  counted as a catalog change and published a delta. One file appended to 20
  times a second in a 2,838-entry folder cost 43% of a core and 191
  publications in 30 s, although search reads only names.

## Decision
**Full scans need a reason.** The writer scans every root at startup, for an
explicit reconcile, on overflow or a failed drain, when scopes cannot describe
the change (more than 1024, or a root itself changed), after a failed pass,
when coverage is missing (no watcher, a replacement that failed, a repair set
too large, an offline root that is back), when the mount table changes, and as
a **backstop** every `repair_ms` (`--repair-ms`, default one hour, `0`
disables). Status reports the cause as `last_full_reason`.

**The repair set.** Each full scan collects recursive scopes inotify cannot
keep current: directories whose watch failed, unreadable directories (or the
parent of a failed stat), and mount points whose filesystem reports only
local changes (NFS, SMB/CIFS, FUSE, 9p, Ceph, AFS, Coda; checked by `statfs`
at each device boundary the crawl reports). Every `rescan_ms` (still 30 s by
default) those scopes go through the ordinary scoped pass. A recursive rescan
settles the repair scopes inside it, and the ones that still fail return.
Offline roots are probed instead and trigger a full scan once they are
readable directories again.

**Mount changes.** The worker polls `/proc/self/mountinfo` for `POLLPRI`
next to the inotify descriptor; a change schedules a full scan.

**Metadata events.** The watch mask drops `IN_CLOSE_WRITE`; events carry
`metadata` for attribute changes. The writer ignores them for files. For a
directory it rescans recursively only when that directory is itself a repair
scope (it could not be listed or watched, so its watched parent reports it),
because only then can new permissions have made something listable.
The store classifies an upsert that keeps a row's identity and kind as a
metadata refresh: committed, but not a catalog change, so nothing is
published, and the row keeps its embedding columns.

## Alternatives considered
- **Keep the 30 s full scan and make it cheaper.** Phase 2 of the plan does
  that for the scans that remain, but even a 1 CPU-s scan every 30 s is work
  that finds nothing while inotify is healthy.
- **No backstop.** inotify does not drop events silently (overflow is
  reported), but bugs, kernel quirks and changes made while a watch was being
  installed exist. One full scan an hour is cheap insurance, and `0` turns it
  off.
- **Diffing the mount table to rescan only affected roots.** More code for a
  rare event; a full scan is correct and becomes cheap in Phase 2.
- **Rescanning every directory whose attributes change.** Correct but wasteful:
  `touch dir`, `rsync -a` and `chmod -R` would rescan whole subtrees for no
  name change. A directory outside the repair set was listable and watched,
  so its contents are already known.
- **Refreshing a written file's row instead of ignoring the event.** Nothing
  reads mtime or size today; this can return if a "recently modified" ranking
  needs it.
- **fanotify.** Whole-filesystem marks need `CAP_SYS_ADMIN`; unprivileged
  fanotify offers per-inode marks like inotify.

## Consequences
- Idle CPU at 213k drops from about 7% of a core to under 0.1%, with no
  SQLite temp-file I/O; the 20 Hz churn costs nothing and publishes nothing.
  Measurements are in the [M7 plan](../m7-plan.md) and
  `tests/bench/results/2026-10-10-m7-phase1.jsonl`.
- Coverage gaps are visible: `repair_scopes` counts them and
  `last_full_reason` explains every full scan.
- Changes that inotify never reports on a healthy local filesystem wait up to
  `repair_ms` instead of 30 s. Network and FUSE mounts keep 30 s repairs.
- The live watcher now lives for an hour between full scans, so slots of
  deleted directories accumulate longer; a full watcher degrades into repair
  scans and, past 256 scopes, full scans. Phase 2 reuses slots.
- The stored `catalog_gen` advances for metadata-only commits while the
  published one does not; publications only need it to increase.
