# delta

> **Status:** Implemented (M6 step 3b)
> **Source:** `src/service/delta.c` · **Header:** `include/torchlight/delta.h`
> **Tests:** `tests/unit/test_writer.c` (compaction), `tests/test_daemon.py` (scoped updates), `tests/bench/bench_daemon.py`

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

## Design

`delta_prepare` loads the rows of the touched ids (missing ones were deleted),
merges them with the kept entries the batch did not touch, and checks bounds:
at most max(`min_entries`, base / `divisor`) delta entries, and whole-snapshot
entry and path-byte limits computed conservatively (removals since the source
are not credited). `TL_LIMIT` tells the writer to compact with a full rebuild;
`TL_STATE` means the source is not the last publication. The snapshot retires
every touched id from the base, so updated entries live only in the delta and
deleted ones disappear. The writer uses `min_entries` 4096 and `divisor` 32:
about 15k entries at a 500k base, which builds in about a tenth of a second.

## Invariants

- Kept entries are ascending by id and describe exactly the live entries whose
  rows changed since the base, at `delta_gen`.
- A prepared snapshot never exceeds what a full rebuild would accept.

## Gotchas

The bound is on entries changed since the base, not per batch: a directory
rename touches every descendant, so it usually triggers compaction.
