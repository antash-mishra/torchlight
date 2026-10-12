# ledger

> **Status:** Implemented (M7 Phase 2): persisted-state map of committed catalog rows
> **Source:** `src/service/ledger.c` · **Header:** `include/torchlight/ledger.h`
> **Tests:** `tests/unit/test_ledger.c`, `tests/unit/test_scan_equivalence.c`

## Purpose
A full scan used to send every crawled entry through SQLite (a lookup, an
upsert and a `seen` insert each) to find the few that changed. The ledger lets
the writer answer "is this entry exactly what the catalog holds?" and "which
rows did this scan not find?" in memory, so a scan that finds nothing changed
writes nothing ([ADR 0040](../../adr/0040-cheap-full-scans.md)).

## Responsibilities
- Hold, for every committed `files` row, its id, a fingerprint of its path and
  its store signature.
- Look a path up and mark the row seen for the current scan; list unseen rows.
- It does **not** read SQLite, decide what to delete or compute signatures:
  the [store](../store/README.md) computes signatures and deletes only rows
  it is told are unseen; the [writer](../writer/README.md) fills and refreshes
  the ledger.

## Public API
| Function | Description |
|---|---|
| `ledger_create()` / `ledger_destroy()` | Empty, incomplete ledger; free |
| `ledger_reset()` / `ledger_seal()` | Start a reload / declare it complete |
| `ledger_put(id, path, signature)` | Record or replace a committed row (new ids must grow) |
| `ledger_remove(id)` | Forget a row (deleted, or about to be reloaded) |
| `ledger_invalidate()` / `ledger_complete()` | Mark untrustworthy / ask |
| `ledger_begin()` | Clear seen marks before a full scan |
| `ledger_find(path)` | Id and signature of the row at path; marks it seen |
| `ledger_unseen(ids)` | Ids of rows not seen since `ledger_begin` |
| `ledger_contains(id)`, `ledger_count()` | Membership, size |

Paths are borrowed for the call and never kept.

## Design
- **Records** `{id, key, check, signature}` (32 bytes) ascend by id. Catalog
  ids come from SQLite `AUTOINCREMENT` and only grow, so new rows append, and
  lookups by id are binary searches. A `ledger_put` with a smaller new id
  returns `TL_STATE` and marks the ledger incomplete.
- **Path fingerprint**: `key` is FNV-1a of the path from the usual seed (the
  hash-index key), `check` FNV-1a from a second seed, compared on every index
  hit. 128 bits make an accidental collision within one catalog practically
  impossible. The store's deletion rules make even a deliberate one harmless:
  it can only keep a stale row.
- **Index**: the core hashmap from `key` to record position.
- **Bits**: `dead` marks removed records (kept in place so ids stay sorted)
  and `seen` the current scan's hits. When removed records are at least 1024
  and half of the storage, compaction drops them and rebuilds the index.
- **Completeness**: `ledger_seal` after a reload declares the ledger complete
  unless a put failed since `ledger_reset`. Any failure or `ledger_invalidate`
  clears it. The writer uses an incomplete ledger for nothing.

## Data flow
`make_snapshot` refills an incomplete ledger from the rows it loads anyway. A
full pass that finds it incomplete reloads it from the reader connection. Each
exact pass calls `ledger_find` per crawled entry. Full passes also call
`ledger_unseen`, which feeds `store_unseen`. After the commit, the writer
removes and reloads (`store_load_ids`) every touched or changed row.

## Invariants
- Records ascend by id; every live record is in the index exactly once.
- When complete, the ledger holds every committed row with the signature the
  store reports for it.

## Performance
About 32 bytes per record plus the index (12 bytes per slot at no more than
50% load): 12.6 MiB at 213k rows (62 bytes each) and 27.2 MiB at 494k. A lookup hashes the
path twice and probes once. See the [M7 plan](../../m7-plan.md) for measured
scan costs.

## Testing
`test_ledger.c`: lookups and seen marks, renames (same id at a new path),
removal and reload in place, misuse (shrinking ids, id 0) leaving it
incomplete, and compaction keeping survivors. `test_scan_equivalence.c`
compares a writer using the ledger with an exhaustive one over random
filesystem changes.

## Gotchas
- A pass that cannot trust the ledger silently writes every entry, as before
  ADR 0040. Correctness never depends on the ledger, only speed.

## Related
- Modules: [store](../store/README.md), [writer](../writer/README.md)
- ADRs: [0040](../../adr/0040-cheap-full-scans.md)
