# watch

> **Status:** Implemented (M2; M6 created flag; M7 metadata flag, filesystem types, slot
> reuse and one long-lived watch set confirmed and swept by full scans): bounded inotify
> collection and rename pairing
> **Source:** `src/fs/watch.c` · **Header:** `include/torchlight/watch.h`
> **Tests:** `tests/unit/test_watch.c`, `tests/unit/test_writer.c`, `tests/test_daemon.py`

`watch_create/add/drain/destroy` own a nonblocking inotify descriptor, directory
paths, descriptor lookup map and 256 pending move cookies. Callers install
directory watches during crawling. One worker owns each watcher; callback paths
borrow only callback lifetime. Directory symlinks are not followed.

Create/delete/move events schedule reconciliation; attribute changes are
flagged as metadata (see M7 below) and writes are not subscribed. Matched move
cookies carry both old/new byte paths, allowing the writer to preserve ids.
Directory moves update watched descendant path mappings before subsequent events
are delivered. Unmatched or ambiguous moves are repaired by scanning. Kernel
overflow and cookie exhaustion discard unreliable pairing state and emit a
reconciliation signal; bounded draining processes at most 256 KiB per turn.

Directory capacity/kernel watch exhaustion returns a counted unavailable status;
the writer puts that directory in its repair set and rescans it periodically.
Instance creation failure also permits scan-only operation: the writer reports
degradation, retains an existing watch set and retries setup on later scans.
Ignored/deleted watches schedule repair. Since M7 Phase 2 one watch set lives
for the daemon's lifetime: full scans confirm and sweep it instead of building
a replacement (see below). Offline/unreadable directories never establish
deletion.

`watch_feed` replays validated kernel-format buffers for deterministic tests.
Tests cover real file/directory cookies, relocated descendant watches, malformed
buffers, overflow, capacity exhaustion, and writer-level overflow repair of an
unwatched subtree. Integration tests cover unavailable scopes and restart repair.
Writer factory-failure tests cover complete instance exhaustion and recovery.

See [writer](../writer/README.md), [crawl](../crawl/README.md) and
[ADR 0011](../../adr/0011-m2-daemon-writer-and-reconciliation.md).

## Created entries (M6 step 3)

Events now carry `created`: set for `IN_CREATE` and for `IN_MOVED_TO` without
a paired source (moved in from an unwatched place). The writer rescans a
created directory recursively, because nobody has seen its subtree; other
events only rescan the parent's children. Tests check a fresh file, a paired
rename (not created) and a directory moved in from outside.

## Metadata events and filesystem types (M7)

Search reads names only, so the watch mask no longer includes
`IN_CLOSE_WRITE`: saving or appending to a file wakes nobody, and a burst of
writes no longer fills the kernel queue next to the creates that matter.
`IN_ATTRIB` stays, because a directory whose permissions change may have
become listable. Events now carry `metadata` for attribute changes (and for a
fed close-write): the entry's name and place are unchanged. The writer ignores
metadata events for files and, for a directory in its repair set, rescans
that directory recursively.

`watch_type_reliable(f_type)` says whether inotify reports every change on a
filesystem type. NFS, SMB/CIFS (including SMB2), FUSE, 9p, Ceph, AFS and Coda
report only changes made through this machine's kernel; anything another
client or the server changes goes unseen. `watch_reliable(path)` applies it to
`statfs(path)`. The writer checks every crawled mount point this way and
repairs unreliable mounts by periodic rescans. Tests cover a real file and
directory chmod, an append that produces no event, a fed close-write, and the
classification of local and remote magic numbers. See
[ADR 0038](../../adr/0038-repair-scans-and-metadata-events.md).

## Slot reuse (M7)

Capacity bounds the directories watched at once, not the slots ever used.
When the kernel drops a watch (`IN_IGNORED`: the directory was deleted, moved
to another filesystem or unmounted), the watcher reports the directory as
before, then frees its path, removes its descriptor from the lookup map
(`hashmap_remove`) and lists the slot for the next `watch_add`. A watch that
gets no slot or lookup entry is removed again with `inotify_rm_watch`, so the
kernel never delivers events nobody can attribute.

Before, dead slots stayed allocated and counted against capacity until a full
scan replaced the watcher. That happened every 30 s before M7 phase 1; since
then it happens at most hourly, and never with `--repair-ms 0`, so a day of
creating and deleting build trees could exhaust the watcher and push the
writer back to full scans. `slot_reuse` in `test_watch.c` creates and deletes a
watched directory five times with room for the root plus one (each cycle after
the first failed with `TL_LIMIT` before) and checks that a reused slot reports
events under its new path. See
[ADR 0043](../../adr/0043-mount-changes-near-roots-and-watch-slot-reuse.md).

## One long-lived watch set (M7 Phase 2)

Full scans used to build a new inotify instance, re-adding every directory
(34,471 `inotify_add_watch` calls and twice the kernel watches at 213k
entries until the swap). Now they reuse the live watcher
([ADR 0040](../../adr/0040-cheap-full-scans.md)):

- `watch_begin` starts a scan generation and resets `unavailable`, which
  therefore counts failures since the last full scan, as a fresh watcher's
  counter did.
- `watch_add(path, identity)` confirms a directory for this generation. A
  second lookup map (path → slot) finds the slot at that path. When it holds
  the same object (device and inode from the crawl), no system call runs.
  Otherwise `inotify_add_watch` runs: the kernel returns the existing
  descriptor for an object watched under another path (moved, events not
  drained yet), whose slot then adopts the new path. A path match alone is
  never trusted: a directory moved out of every root keeps its watch under
  the stale path, and a new directory created there must get its own.
- Paired directory moves (`relocate`) re-key the moved slots and confirm them.
- `watch_sweep(keep)` removes the watches no add or move confirmed since
  `watch_begin` (`inotify_rm_watch`, slot freed; the IN_IGNORED that follows is
  dropped silently), except those keep spares (below roots the scan could not
  read).

Tests cover the skipped system call (a confirmed add succeeds on a mode-0
directory, where `inotify_add_watch` fails), sweeping (an unconfirmed
directory stops reporting events, a confirmed one again later) and a new
directory under a stale path (gets its own watch; the stale one is swept).
