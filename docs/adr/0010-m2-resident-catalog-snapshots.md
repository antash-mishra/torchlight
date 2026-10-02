# 0010. Start M2 with bounded resident catalog snapshots

- **Status:** Accepted
- **Date:** 2026-10-02
- **Refines:** 0003, 0004 and 0005

## Context

M1 seals an immutable lexical engine, but every CLI query reloads it. M2 needs
resident queries and publication during updates. Its first prerequisite is safe
snapshot ownership: a reader must not acquire a freed view, and releasing the
last reader must not free a large index on the interactive thread. Catalog rows
and persisted `catalog_gen` also need to come from the same committed view.

## Decision

Add an index-layer `catalog` module wrapping sealed lexical engines. Prepare a
bounded pool of reader workspaces with each snapshot, then publish under a
short lifecycle mutex. Acquiring an exclusive reader slot pins the same view
under that lock. Queries run outside it. Publication retires the previous view;
background reclamation detaches unleased views and frees them outside the lock.
Keep detached views counted until destruction completes. Reject equal/older
`catalog_gen`s and publication beyond the configured snapshot capacity.

Keep SQL in `store.c`. `store_load_catalog` opens a SQLite read transaction and
streams committed rows with their persisted `catalog_gen`, even while another
WAL connection commits. Failed callbacks discard the read transaction and leave
the output `catalog_gen` unset. Validate metadata as decimal integers within
SQLite's signed range; exhausted counters reject commits rather than saturate.

Add binary-search file-id resolution to the sealed lexical engine. Snapshot
leases retain exact byte paths; the future daemon acquires the current view
again for launch resolution. Use platform pthreads for synchronization; no new
external package is required.

## Alternatives considered

- Load an atomic active pointer then increment its reference: races reclamation.
- Allocate scratch on acquisition: violates the interactive allocation budget.
- Destroy on last release: puts potentially large frees on the query thread.
- Read `catalog_gen` separately from rows: can mislabel a concurrent update.
- Add IPC before establishing reader ownership: makes lifecycle bugs harder to
  isolate. IPC remains the next integration increment.

## Consequences

This is an M2 foundation, not a runnable daemon. The CLI keeps its existing local
behavior. Socket protocol, writer orchestration/history, watch/reconciliation,
client deadlines and daemon status still need implementation. No schema change
or query ranking change is introduced. Full engine rebuilds are the initial
snapshot baseline; count bounds do not establish a total memory budget. Future
writer code must prepare changes before commit, limit staging, publish afterward,
and recover post-commit publication failure before accepting later updates.
