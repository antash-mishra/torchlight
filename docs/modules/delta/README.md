# delta

> **Status:** Implemented (M6 step 3b; M7 deletion-compaction optimization removed; M7 Phase 3 slack for deferred compaction)
> **Source:** `src/service/delta.c` · **Header:** `include/torchlight/delta.h`
> **Tests:** `tests/unit/test_delta.c` (compaction accounting and leases), `tests/unit/test_writer.c`,
> `tests/unit/test_writer_repair.c` (incremental deletions and release), `tests/test_daemon.py`, `tests/bench/bench_scenarios.py`

## Purpose

Incremental catalog publication for the writer's indexing thread. It keeps
owned copies of every entry changed since the published base engine and turns
each committed change set into the next [catalog](../catalog/README.md)
snapshot, so a small update builds a small engine instead of the whole catalog.

## Responsibilities

- Owns the kept delta entries (ascending ids) and a pending list for one
  publication in flight.
- Loads committed rows by id from the store's read connection, builds the delta
  engine with `lexical_set_reference(base)` and derives the snapshot.
- Does **not** decide when to rescan, own SQLite writes or publish: the
  [writer](../writer/README.md) does that.

## Public API

| Function | Description |
|---|---|
| `delta_create()` / `delta_destroy()` | Owned, empty bookkeeping. |
| `delta_reset(delta, gen, entries, bytes)` | A full rebuild published a new base; forget kept entries. |
| `delta_gen(delta)` | `catalog_gen` of the last publication described. |
| `delta_prepare(delta, store, source, ids, count, limits, &out)` | Build the snapshot after `source` for a committed batch that touched `ids`. |
| `delta_commit()` / `delta_abandon()` | Adopt or drop the prepared entries after publication. |
| `delta_oversized(delta, limits)` | Whether the adopted delta is past its bound, so a compaction should follow. |

## Design

`delta_prepare` loads the rows of the touched ids (missing ones were deleted),
merges them with the kept entries the batch did not touch, and checks bounds:
at most max(`min_entries`, base / `divisor`) live delta entries, and whole-snapshot
entry and path-byte limits computed conservatively (removals since the source
are not credited). `TL_LIMIT` tells the writer to compact with a full rebuild;
`TL_STATE` means the source is not the last publication. The snapshot retires
every touched id from the base, so updated entries live only in the delta and
deleted ones disappear. The writer uses `min_entries` 4096 and `divisor` 32:
about 15k entries at a 500k base, which builds in about a tenth of a second.

`limits.slack` (M7 Phase 3,
[ADR 0045](../../adr/0045-settled-bursts-and-deferred-compaction.md)) lets a
delta grow to `slack` times its bound before `TL_LIMIT`. The writer uses 4: a
22k-entry burst or directory rename at 213k (bound 6.6k) is published as a
delta in about 0.2 s instead of a 1.6 s rebuild. After `delta_commit`,
`delta_oversized` tells the writer that the kept entries exceed the bound, and
the writer compacts them with a full rebuild once it is idle.

The M7 optimization that also counted deleted base rows was removed: it made
bulk deletion wait for full rebuilds. Base tombstones no longer count toward
the compaction threshold. The [withdrawn ADR 0041](../../adr/0041-compaction-counts-retired-base-entries.md)
records the measurements and the remaining memory issue.

Regressions check that deletion in one batch or across batches stays
incremental even above the threshold, while live delta entries still compact
when they exceed it, and that with slack a delta past its bound publishes and
reports itself oversized until slack times the bound. They also cover repeated updates, mixed changes,
abandonment and simultaneous old/new readers observing their own snapshots.

## Invariants

- Kept entries are ascending by id and describe exactly the live entries whose
  rows changed since the base, at `delta_gen`.
- A prepared snapshot never exceeds what a full rebuild would accept.
- An adopted delta holds at most `slack` times its bound; more than the bound
  only until the writer's deferred compaction runs.

## Gotchas

The bound covers live changes since the base, not per batch. Directory renames
can still trigger compaction, while deleted base rows remain allocated until
a later rebuild. A full engine rebuild reads the committed catalog; it does
not require a full crawl. Deferred deletion compaction is not implemented.
