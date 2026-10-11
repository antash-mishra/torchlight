# 0043. Full scans only for mounts near a root; reuse dropped watch slots

- **Status:** Accepted
- **Date:** 2026-10-11
- **Refines:** 0038 (repair scans: "a change schedules a full scan"; "Diffing
  the mount table ... more code for a rare event"), 0012 (watch fallback)

## Context
ADR 0038 schedules a full scan of every root whenever `/proc/self/mountinfo`
signals a change, and rejected diffing the table because mount changes were
assumed rare. On the reference Linux Mint desktop they are not: Timeshift's
cron job (`0 * * * * timeshift --check`) mounts the backup disk under
`/run/timeshift/<pid>/backup` at :00:01 and unmounts it at :00:18. The live
daemon ran exactly five full scans in two hours: startup plus these four.
Each cost about 3 CPU-s and 48 MiB written at 213k entries, and kept resetting
the hourly backstop. Snaps, containers and removable media do the same on
other desktops. None of these mounts is near the indexed home folder.

Fewer full scans expose a second problem. The watcher kept the slot of every
directory whose watch the kernel dropped (deleted, moved off the filesystem,
unmounted) and counted it against capacity until a full scan replaced the
watcher. With full scans at most hourly, or never with `--repair-ms 0`,
directory churn could fill the watcher, make `watch_add` fail, grow the repair
set and finally bring back full scans every `rescan_ms`.

## Decision
- **Diff the mount table.** A new `src/fs` module, `mounts`, reads the table
  into a snapshot keyed by each line's first five fields (mount id, parent id,
  device, root, mount point; options ignored, so a remount is no change) with
  decoded mount points. After each signal the writer reads a new snapshot and
  asks `mounts_affect` whether a mount present in only one of the two lies at,
  below or above a configured root. Only then does it schedule a full scan
  (`last_full_reason: mount`). An unreadable table counts as relevant.
  `mount_changes` in status counts every signal examined.
- **Reuse dropped watch slots.** On `IN_IGNORED` the watcher reports the
  directory as before, then frees its path, removes its descriptor from the
  lookup map (new `hashmap_remove`, backward-shift deletion) and lists the slot
  for reuse. Capacity counts live watches. A watch that cannot get a slot or
  lookup entry is removed from the kernel again.

## Alternatives considered
- **Coalesce mount signals into one full scan a minute later.** Halves
  Timeshift's cost but still scans every root for mounts that concern none.
- **Rescan only the mount point (scoped) for mounts below a root.** Saves a
  full scan for the rare relevant mount, at the cost of handling mounts above
  a root differently. A full scan is correct and rare once unrelated mounts
  are ignored, and Phase 2 makes it cheaper.
- **Compare mount points only.** Misses a different device mounted at the
  same place between two reads, and a mount id reused elsewhere.
- **Replace the watcher on a timer to drop dead slots.** Rebuilds 34k
  watches for nothing; reuse is local and immediate.

## Consequences
- On the reference desktop the two hourly Timeshift scans go away; the hourly
  backstop is again the only routine full scan.
- A mount inside a root (a USB disk under the home folder) still triggers a
  full scan, as does any change seen through an unreadable table.
- The watcher's slot vector no longer grows with directory churn; deleted
  directories cost nothing after their `IN_IGNORED`.
- `tl_writer_options.mounts_path` lets tests supply the table; the M7 scenario
  benchmark's `mounts` phase mounts tmpfs in a private user and mount
  namespace (`unshare`, `nsenter`), outside and inside the root.
