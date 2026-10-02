# 0011. Complete M2 with a bounded lexical daemon and reconciled updates

- **Status:** Accepted
- **Date:** 2026-10-02
- **Refines:** 0003, 0004, 0005 and 0010

## Context

The resident catalog lifecycle is implemented, but local CLI queries still load
SQLite and rebuild an engine. M2 needs a runnable service, byte-safe transport,
live filesystem updates, asynchronous history and recovery around publication.
The sealed lexical engine currently has no incremental mutation/block-sharing API.

## Decision

Add reusable orchestration in `src/service/`, above IPC, filesystem, storage and
index modules. Both executables remain wiring/argument parsing code. One Linux
poll loop searches the resident catalog and handles nonblocking sockets; one
worker owns SQLite, crawling, inotify and reclamation. SIGINT/SIGTERM are consumed
through a signalfd after blocking them before worker creation. Socket and
canonical database lock files prevent duplicate daemons and offline index races.
Keep lock files after closing to avoid splitting a lock across two inodes.

Use a generic caller-buffer JSON codec with depth/token/string bounds and the
existing utf8proc dependency. No external dependency is added. Version 1 rejects
unknown/duplicate request fields and invalid UTF-8/NUL. File ids are decimal
strings; results carry exact UTF-8 `path` or base64 bytes, with separate display.
M2 supplies one `final` response and reserves `emb_gen: null` for M4.

Preallocate sixteen client buffers, each with 8 KiB input and 1 MiB total queued
output. Bound active ids to four and require progress within a five-second
deadline. Release leases after encoding, before socket transmission. Reject
duplicate active ids and coalesce query frames already buffered from one client,
completing superseded queries as cancelled. An older client's work cannot cancel
another client's query. No query invokes SQLite, filesystem reads or allocation.

Coalesce inotify changes for 100 ms and reconcile configured roots in a bounded
transaction. Install a new watch set during crawling while keeping the previous
set live. Drain the old set before pruning and use paired cookies to preserve
file and descendant ids. Remap pending seen/kept paths for late moves. Build the
full candidate and reader workspace privately from the pending SQLite view;
only then commit and publish. Roll back preparation/commit errors and retain
paired moves for the retry to preserve rename identity. After a
post-commit publication error, reload committed SQLite before another catalog
batch. Keep serving the old catalog with degraded/recovering status meanwhile.
No-change scans roll back scratch/sequence bookkeeping and avoid rebuilding.

Bound catalog entries and aggregate path bytes (defaults 500k and 128 MiB),
watch slots (65,536), paired moves (256), retained snapshots (two) and staged
engines (one). Staging starts only after retired views have been reclaimed.
This provides structural bounds, not a hard process RSS cap. Full engines are
rebuilt; shared index blocks remain a future performance optimization.

Queue at most 256 optional history events in FIFO order. Assign a random session
id plus monotonic sequence before responding. Persist searches before opens,
deduplicate launch-event ids, and use NULL for unretained searches. Saturation
drops history with a counter; catalog reconciliation uses one coalesced flag
instead of a lossy event queue. History can be disabled, cleared asynchronously
or retained for a configured number of days. Shutdown drains within a bounded
deadline; unavailable history writes increment failure/drop counters.

Periodic scans (default 30 seconds), startup scans, watcher overflow, exhausted
watch capacity and explicit reconciliation repair missed events. Unreadable
scopes/unresolved root identities retain saved entries. Status distinguishes
indexing, recovering, offline/unreadable scopes, reduced watch coverage and
history queue counters.

## Alternatives considered

- cJSON: planned, but a small bounded protocol does not require a new package.
- Filesystem and SQL in the query loop: makes interaction wait on I/O/locks.
- Mutating a sealed engine: violates snapshot lifetimes and narrowing caches.
- Commit before constructing a candidate: turns allocation failures into a
  routinely stale catalog and increases the recovery window.
- Dropping filesystem events when a queue fills: silently loses live updates.

## Consequences

M2's service/protocol/update/history behavior is implemented and covered by
sanitizer, concurrent-client, crash/restart and injected-failure tests. The
resident benchmark measures startup, first query, engine and socket latency,
indexing-load latency, publication lag and RSS. Full rebuilds make large updates
cost more than small deltas would. The existing 500k / 5 ms lexical performance
gate remains a measured limitation; functional M2 completion does not close it.
GTK launch actions/systemd integration remain M3, semantics M4 and ranking
personalization M5.
