# writer

> **Status:** Implemented (M6 steps 1 and 3; M5 batched history and usage load): indexing and persistence threads, private
> scan batches, scoped rescans of event directories, delta publication with compaction;
> M2/M3 reconciliation contracts retained
> **Source:** `src/service/writer.c` · **Header:** `include/torchlight/writer.h`
> **Tests:** `tests/unit/test_writer.c`, `tests/unit/test_writer_fallback.c`, `tests/unit/test_writer_history.c`, `tests/test_daemon.py`

The writer owns two threads and two SQLite connections. The indexing thread
drains inotify, crawls selected roots into a private batch (copied entries,
kept offline roots and rename pairs), hands it to the persistence thread and
waits, then builds the engine from the committed rows on its own read-only
connection and publishes the snapshot. The persistence thread owns the write
connection: it applies each batch in one short transaction, drains the history
ring and runs retention. No crawl or index build runs inside a write
transaction, so history writes wait at most for one batch commit. Creation
loads/publishes the saved catalog before starting either thread. Configuration
and the catalog registry remain caller-owned; the registry must have room for
two views. The mutex protects status, the history ring and the batch handoff,
never I/O or builds. See
[ADR 0028](../../adr/0028-m6-search-thread-and-persistence-owner.md).

An event burst schedules one reconciliation, coalesced for 100 ms. The worker
resolves/deduplicates configured roots each time, installs a replacement watch
set while crawling, applies paired renames, and prunes only successfully scanned
scopes. Unreadable subtrees and unresolved root aliases retain saved rows.
The old watcher stays live until the scan finishes; events during crawling
schedule another pass. Periodic reconciliation defaults to 30 seconds and repairs
unwatched scopes, overflow, missed events and restart changes.

Failure to create an inotify instance no longer blocks reconciliation. TL_IO
increments watch-unavailable status, marks coverage degraded and continues
scanning without a replacement watcher. Any existing watcher stays live.
Later periodic/explicit scans retry setup; a successful replacement restores
watch coverage. Memory or invalid-input failures still roll back the batch.

Scans compare persisted filesystem incarnations before upserting paths, so a
replacement retires stale ids even when notifications were coalesced or missed.
Directory replacement retires descendants; failed batches restore ids/history.
Paired moves retain ids subject to validation of the filesystem object key.

Catalog changes are private: crawl into a batch, apply renames/upserts/prune in
one transaction, commit, then build the committed view and publish its
`catalog_gen`. SQL failure rolls back the batch and retains paired moves for the
retry, so renamed files keep their ids after a failed transaction. Renames
received before the crawl apply before upserts; those received during it apply
after upserts and before pruning. A failed build or publication after the commit
sets recovering/degraded status and gates later catalog batches on committed reload.
The old view remains searchable. No-change scans roll back temporary bookkeeping
and avoid a rebuild. Snapshot exhaustion retries after reclamation.

Entry count and aggregate path bytes bound each catalog (defaults 500k/128 MiB);
two retained views, one staged candidate, 65,536 watch slots, 256 rename pairs
and 1024 event scopes bound outstanding work. Staging starts with no retained
retired view. These are structural limits, not a hard RSS budget.

The FIFO history ring holds 256 copied query/open/clear events. Search ids are
assigned in memory by the daemon. Accepted searches precede their opens, retries
deduplicate by event id, and missing retained searches become NULL. Full history
queues increment a drop counter; failed writes increment a failure counter.
History is optional, retention runs periodically, and clear also works when
recording is disabled. A written counter complements pending/dropped/failures.
Shutdown interrupts scans, rolls back unfinished work, joins the indexing thread,
then lets the persistence thread answer any pending batch and drain history
within a five-second deadline (a currently blocked SQLite operation can add its
bounded busy timeout).

Publication and event-source adapters permit deterministic failure/overflow
replay without production environment-variable test modes. Tests hold a committed
candidate before failed publication, query the old view, exhaust history slots
and watch them drain while publication is still blocked, gate subsequent catalog
writes, recover by reload and repair an unwatched subtree after replayed overflow.

The watcher factory adapter additionally validates periodic publication with no
inotify instance, replacement identity without notifications, restoration of
watching, and preservation of the old watch set on later factory failures.

See [ADR 0011](../../adr/0011-m2-daemon-writer-and-reconciliation.md),
[ADR 0012](../../adr/0012-filesystem-incarnations-and-watch-fallback.md),
[store](../store/README.md), [watch](../watch/README.md), [catalog](../catalog/README.md).

M3 copies resolved desktop ids into internal history events. The same FIFO writer
uses store_desktop_open for desktop launches and store_open_event for files.
No-history, retention, deduplication, clear and diagnostic counters cover both.
These writes do not change catalog_gen. Immutable directory flags are loaded
alongside file paths for presentation, with no query-time filesystem lookup.

## Scoped reconciliation (M6 step 3a)

Watch events become directory **scopes** instead of a bare dirty flag: the
parent of a changed entry (and of a rename's source) is rescanned for its
direct children, and a directory that appeared (`created`) is rescanned
recursively. Before the pass, scopes nested in a recursive scope are dropped and
every scope must be indexed by some root (`crawl_covers`). `crawl_scope` fills
the batch and installs watches for new directories into the live watcher; the
persistence thread applies renames and upserts, then `store_prune_children` or
`store_prune_tree` per scope that could be listed. A scope that cannot be
listed (deleted meanwhile) is not pruned; its parent's event accounts for it.

A full scan of every root still runs at startup, for overflow, explicit
requests, periodic repair, any failure, more than 1024 scopes, a missing
watcher and any scope above a root (the root itself changed). Events arriving
during a pass start a fresh scope set. `scoped_reconciliations` counts scoped
passes. A daemon test runs with periodic repair disabled through creations,
deletions, a tree moved in from outside the roots, a paired directory rename,
a deleted subtree and a hidden directory created next to a visible one (a
use-after-free regression in scope resolution).

## Delta publication (M6 step 3b)

The persistence connection tracks the files rows each catalog transaction
touches (`store_track_changes`, up to 131072). After a commit it copies them
for the indexing thread, which publishes through the [delta](../delta/README.md)
module: it loads the committed rows, merges them into the kept delta entries,
builds a small engine that references the base and derives the next snapshot,
retiring the touched ids from the base. A full rebuild compacts instead when the
delta would exceed max(4096, base/32) entries (`tl_writer_options.delta_entries`
lowers the minimum for tests), when tracking overflowed, when registered roots
changed or when the active view is not the writer's last publication. Startup,
reload after failed publication and compaction use full rebuilds.
`delta_publications` and `full_builds` count both paths. At 500k entries, 100
touched files publish in about 110 ms (12 ms of work after the 100 ms coalescing
window) with RSS growing about 1%, against 4 s and a doubled engine before. See
[ADR 0030](../../adr/0030-m6-incremental-indexing.md).

## Batched history and the startup usage load (M5)

The persistence thread still does all SQLite work for history; the search
thread only queues events ([ADR 0033](../../adr/0033-m5-personal-ranking.md)).

- **Drains.** Each drain writes all popped events in one transaction. A clear
  commits what came before it and runs on its own. Dropped events and
  rejected events are counted at once; written events, and failures from a
  lost commit, are counted when the batch ends. `history_pending` counts
  queued events only, not those of a batch still being written.
- **Failures.** An event that fails with an SQL error ends the batch at once:
  SQLite may have rolled the whole batch back (an I/O error, out of memory),
  so its earlier events are committed or counted as lost, and later events
  start a new batch. If the batch cannot begin, because another connection
  holds the database lock, the drain writes each event alone and does not
  retry the begin, so a lock costs one busy timeout per event (as before
  batching) rather than two.
- **Fewer events.** The daemon no longer queues a search per query. An open
  carries its search's query instead, and the store saves both rows together.
- **Startup load.** Before draining any event, the persistence thread
  rebuilds a [usage](../usage/README.md) summary from the retained opens.
  `writer_take_usage` offers it once. Because the load reads history before
  any new event is written, the opens the search thread recorded meanwhile
  are not in it, and the [personal](../personal/README.md) state merges them
  without double counting. A failed load counts as a history failure; the
  live summary simply starts empty. Retained rows the store never writes are
  skipped, so one bad row cannot empty the summary.

`test_writer_history.c` checks the startup load (a bad row skipped, the
summary offered exactly once), the counters when SQLite loses a whole batch
(the event after the loss is written in a new batch), and that a drain
blocked by another connection's lock waits one busy timeout per event plus
one failed begin (about 9 s for two events, against 12 s when the begin was
retried per event).
