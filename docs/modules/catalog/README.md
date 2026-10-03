# catalog

> **Status:** Implemented (M2/M3): resident snapshot lifecycle and directory metadata;
> integrated with the M2 daemon/writer
> **Source:** `src/index/catalog.c` · **Header:** `include/torchlight/catalog.h`
> **Tests:** `tests/unit/test_catalog.c`

## Purpose and ownership

Wrap sealed lexical engines in immutable resident views identified by
`catalog_gen`. This module depends on the lexical engine and platform pthreads;
it performs no storage or filesystem operations. The background writer
supplies committed views and the query loop leases them.

`catalog_snapshot_create` prepares all reader workspaces before publication and
transfers engine ownership only on success. `catalog_publish` transfers snapshot
ownership only on success and rejects equal/older `catalog_gen`s. Unpublished
views remain caller-owned and can be destroyed or retried. A fresh database's
empty `catalog_gen = 0` is valid for first publication.

Large-engine workspaces also own fixed lexical scoring workers, started during
snapshot preparation. Thread startup failure returns `TL_IO` without transferring
the engine. Reclaiming an unleased snapshot joins those workers outside the
lifecycle lock; acquisition/release never starts or stops threads.

## Reader lifecycle

Acquire a reader lease, query/resolve through it, and release exactly once.
Each lease owns exclusive use of a preallocated workspace; different leases
query concurrently without a shared query lock. Result and resolved paths
borrow the lease's lifetime, including when the view has been retired.
Acquiring the active pointer and pinning its view happen under the same short
lifecycle mutex, preventing reclamation between those two operations.

Resolve against a newly acquired current view before launching. A lease from an
earlier query intentionally still sees its earlier path/id mapping. Missing ids
return `TL_STATE`; raw paths, including non-UTF-8 bytes, are returned unchanged.
The module validates the catalog mapping, not current filesystem existence.

Release only updates lifecycle counters. It never destroys engines/workspaces
on an interactive thread. The writer calls `catalog_reclaim`, which detaches
unleased retired views under the mutex and frees them outside it. Detached views
continue counting toward capacity until destruction completes. Destroy the
registry after joining all workers; destruction rejects outstanding leases.

## Bounds and failure behavior

The caller configures 1–64 retained snapshots and 1–64 reader slots per view.
Acquire fails with `TL_LIMIT` when the active view's slots are busy. Publication
fails with `TL_LIMIT` when retained views exhaust capacity; the active view
continues serving, and the candidate remains owned by the writer. Reclaiming
unleased retired views makes room for retry. Updating a nonempty registry needs
capacity of at least two.

These are count bounds, not a total RSS budget: a candidate being built is also
caller-owned, and every view currently rebuilds the full engine. The M2 writer
limits staging to one candidate and bounds entries/path bytes; the daemon
enforces client/output deadlines. Shared index blocks and a hard process RSS
budget remain future work.
Scratch/cache contents are tied to one immutable engine, so publication cannot
reuse old subsequence membership for a changed catalog.

## Validation and measurements

Tests cover ownership on failed preparation/publication, monotonic publication,
reader/snapshot exhaustion, multiple leases holding retired paths, stale ids,
empty initial state, and rejected destruction while leased. A barrier test uses
four query threads across 24 replacements; old views remain searchable during
publication and reclamation, then disappear after every reader releases them.
Another four-reader test races acquisition/query/release against publication and
reclamation, checking every result and resolved id against its pinned catalog_gen.

`make bench` also reports `catalog_query_ms`: warm whole-query latency including
lease acquisition and release over the held-out queries, excluding construction,
SQLite and IPC. No daemon round-trip performance is claimed.

## Related

- [ADR 0010](../../adr/0010-m2-resident-catalog-snapshots.md)
- [lexical](../lexical/README.md), [store](../store/README.md),
  [daemon](../daemon/README.md), [evaluation](../../evaluation.md)

M3 catalog_is_dir reads immutable directory metadata while leased. It returns
false for absent/unleased entries and performs no I/O/allocation. Directory
flags originate in store_load and the writer's lexical_add_entry builder.
