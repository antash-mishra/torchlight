# catalog

> **Status:** Implemented (M2/M3; M6 step 3 segmented snapshots; M5 personal boosts): resident snapshot lifecycle,
> directory metadata, shared bases with delta segments; integrated with the daemon/writer
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
caller-owned. Since M6 a derived view shares its base engine instead of
rebuilding it (see below), so a small update adds only a delta engine, a
tombstone bitmap and a live-entry map. The writer limits staging to one
candidate and bounds entries/path bytes; the daemon enforces client/output
deadlines. A hard process RSS budget remains future work.
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

## M4 metadata integration

Metadata pins retain snapshots without reserving lexical workspaces; scoped
context paths exclude parents outside the indexed roots. Background consumers
copy metadata, release pins and reclaim through the existing lifecycle.

## Cancellation passthrough (M6 step 1)

`catalog_reader_cancel` attaches a cancellation flag to a lease's workspace
(see the lexical module). The daemon sets it per query lease so a superseded
search stops early and the lease is released promptly.

## Segmented snapshots (M6 step 3)

A snapshot is a **base** engine, shared and reference-counted by every snapshot
derived from it, plus an optional **delta** engine of all entries changed since
that base and a **tombstone** bitmap of base positions they replace or remove.
`catalog_snapshot_derive(source, &delta, retired_ids, ...)` prepares the next
view: it shares `source`'s base, adds the base positions of `retired_ids` to
the tombstones and takes the delta (NULL when only removals happened). The
base owns the reader workspaces, so their word caches stay warm across
updates; a lease takes one free base workspace plus the snapshot's reader slot,
and the base's reader capacity bounds concurrent leases across all snapshots
sharing it. Each lease attaches its snapshot's tombstones to the base workspace
and clears them (and the cancel flag) on release.

`catalog_query` queries the base (tombstones excluded) and the delta, then
merges both ordered lists by score, raw path bytes and id, so results equal one
engine holding the live entries. `catalog_resolve` and `catalog_is_dir` look in
the delta first and hide tombstoned base entries. Statistics count live
entries; `catalog_snapshot_entry`/`context` enumerate live entries in id order
through a 4-byte-per-entry map. The base is freed with its last snapshot.
Tests cover shared bases, retirement of the base snapshot, merged order across
segments at equal scores, capacity-limited leases, hidden and renamed entries
and live enumeration. See [ADR 0030](../../adr/0030-m6-incremental-indexing.md).

## Personal boosts (M5)

`catalog_query_boosted` takes boosts by catalog id (`tl_catalog_boosts`). A
lease maps the ids to base or delta positions once per `boosts->key`: absent
and tombstoned ids are skipped, and a changed id lives in the delta. Each
query then splits the boosts into per-segment lists and attaches them to the
base and delta workspaces before querying both. The segments return boosted
scores on one scale, so the existing merge stays exact. `catalog_query` is
the same call without boosts, and every query replaces the previous boosts,
so none outlive their query. `catalog_release` clears them before the shared
base workspace returns to the pool. Each reader preallocates 16 KB of
positions and 64 KB of boost lists. `test_catalog.c` covers delta and base
ids, retired ids, key reuse and remapping, and clearing across leases.
