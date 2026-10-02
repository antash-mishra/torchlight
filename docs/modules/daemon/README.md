# daemon

> **Status:** Planned
> **Source:** `src/bin/torchlightd.c` · **Header:** `—`
> **Tests:** `tests/unit/test_daemon.c`

## Purpose
`torchlightd`: wires all modules together, owns threads, serves queries.

The resident executable is still planned. Its first prerequisite is implemented
in [catalog](../catalog/README.md): immutable lexical views, bounded reader
leases, publication and background reclamation. `store_load_catalog` provides
coherent committed views for startup/recovery. See [ADR 0010](../../adr/0010-m2-resident-catalog-snapshots.md).

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
- Startup loads the saved catalog, serves it, and reconciles in the background.
- Writer prepares private deltas, commits catalog batches, then publishes
  immutable `catalog_gen`s. Failed publication after commit requires reload/rebuild
  before later writes, with degraded status while old snapshots serve queries.
- Queries pin one consistent catalog generation (`catalog_gen`); old blocks are reclaimed after release.
- Pin acquisition and reclamation share a short lifecycle lock or validated
  epoch protocol; pointer load followed by reference increment alone is unsafe.
  Free retired blocks in the background outside the lifecycle lock.
- Assign search ids in memory before responding and enqueue history once per query.
- Reply with lexical results first; run query embedding/vector search and send
  the fused `final` response afterwards. Cancel it if a newer request arrives.
- Both phases pin the same snapshot pair. Lexical-only/error/deadline fallback
  supplies a terminal response; M2 runs lexical-only, M4 adds semantic execution.
- Bound clients, scratch, caches, in-flight inference, pinned snapshots, and output
  queues. Use nonblocking I/O; a slow client cannot stall other clients.
- Cancellation is per client/request; queued stale inference is discarded and
  already-running stale output ignored. Prioritize interactive semantic work
  over background path embedding.
- Query thread never waits on SQLite or background index construction. Embedding
  compute/allocations are measured separately from lexical/ranking guarantees.
- Serialize daemon instances, report indexing state, and shut down/restart cleanly.

## Data flow
_TODO after implementation._

## Invariants
_TODO after implementation._

## Performance
_TODO: complexity, memory use, `make bench` numbers with date._

## Testing
_TODO after implementation._

## Gotchas
_TODO after implementation._

## Related
- [ipc](../ipc/README.md)
- [store](../store/README.md)
- [rank](../rank/README.md)
