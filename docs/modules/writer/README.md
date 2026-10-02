# writer

> **Status:** Implemented (M2): async reconciliation, publication and history
> **Source:** `src/service/writer.c` · **Header:** `include/torchlight/writer.h`
> **Tests:** `tests/unit/test_writer.c`, `tests/test_daemon.py`

The writer owns one SQLite connection and worker thread. Creation loads/publishes
the saved catalog before starting the worker. Configuration and the catalog
registry remain caller-owned; the registry must have room for two views. All
crawls, index construction, commits and reclamation happen off the query thread.
Its mutex protects only short status/history operations, never I/O or builds.

An event burst schedules one reconciliation, coalesced for 100 ms. The worker
resolves/deduplicates configured roots each time, installs a replacement watch
set while crawling, applies paired renames, and prunes only successfully scanned
scopes. Unreadable subtrees and unresolved root aliases retain saved rows.
The old watcher stays live until the scan finishes; events during crawling
schedule another pass. Periodic reconciliation defaults to 30 seconds and repairs
unwatched scopes, overflow, missed events and restart changes.

Catalog changes are private: scan/upsert/prune, build and validate a candidate
from the pending transaction, commit, then publish its `catalog_gen`. Preparation
or SQL failure rolls back the batch and retains paired moves for the retry, so
renamed files keep their ids after a failed transaction. Failed post-commit publication sets
recovering/degraded status and gates later catalog batches on committed reload.
The old view remains searchable. No-change scans roll back temporary bookkeeping
and avoid a rebuild. Snapshot exhaustion retries after reclamation.

The initial M2 strategy scans configured roots and rebuilds complete engines.
Entry count and aggregate path bytes bound each catalog (defaults 500k/128 MiB);
two retained views, one staged candidate, 65,536 watch slots and 256 rename pairs
bound outstanding work. Staging starts with no retained retired view. These are
structural limits, not a hard RSS budget. Shared blocks/incremental rebuilds are
future performance work; large updates incur full build cost.

The FIFO history ring holds 256 copied query/open/clear events. Search ids are
assigned in memory by the daemon. Accepted searches precede their opens, retries
deduplicate by event id, and missing retained searches become NULL. Full history
queues increment a drop counter; failed writes increment a failure counter.
History is optional, retention runs periodically, and clear also works when
recording is disabled. Shutdown interrupts scans, rolls back unfinished work,
joins the worker and drains history within a five-second deadline (a currently
blocked SQLite operation can add its bounded busy timeout).

Publication and event-source adapters permit deterministic failure/overflow
replay without production environment-variable test modes. Tests hold a committed
candidate before failed publication, query the old view, exhaust history slots,
gate subsequent catalog writes, recover by reload and repair an unwatched subtree
after replayed overflow.

See [ADR 0011](../../adr/0011-m2-daemon-writer-and-reconciliation.md),
[store](../store/README.md), [watch](../watch/README.md), [catalog](../catalog/README.md).
