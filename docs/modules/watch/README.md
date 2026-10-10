# watch

> **Status:** Implemented (M2; M6 created flag; M7 metadata flag and filesystem types): bounded inotify
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
Ignored/deleted watches schedule repair. The worker keeps the old watch set live
while installing its replacement during a scan, then drains it before pruning.
Events on the new set drive subsequent reconciliation. Offline/unreadable
directories never establish deletion.

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
