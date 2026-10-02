# watch

> **Status:** Implemented (M2): bounded inotify collection and rename pairing
> **Source:** `src/fs/watch.c` · **Header:** `include/torchlight/watch.h`
> **Tests:** `tests/unit/test_watch.c`, `tests/unit/test_writer.c`, `tests/test_daemon.py`

`watch_create/add/drain/destroy` own a nonblocking inotify descriptor, directory
paths, descriptor lookup map and 256 pending move cookies. Callers install
directory watches during crawling. One worker owns each watcher; callback paths
borrow only callback lifetime. Directory symlinks are not followed.

Create/delete/metadata/write/move events schedule reconciliation. Matched move
cookies carry both old/new byte paths, allowing the writer to preserve ids.
Directory moves update watched descendant path mappings before subsequent events
are delivered. Unmatched or ambiguous moves are repaired by scanning. Kernel
overflow and cookie exhaustion discard unreliable pairing state and emit a
reconciliation signal; bounded draining processes at most 256 KiB per turn.

Directory capacity/kernel watch exhaustion returns a counted unavailable status;
the writer continues scanning with periodic reconciliation for reduced coverage.
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
