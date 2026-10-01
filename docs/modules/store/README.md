# store

> **Status:** M1 catalog implemented; async writer/history APIs planned
> **Source:** `src/storage/store.c` · **Header:** `include/torchlight/store.h`
> **Tests:** `tests/unit/test_store.c`

## Behavior and ownership

`store_create` opens SQLite with WAL, foreign keys and a bounded busy timeout.
All SQL lives in this source file, is constant, and is prepared before execution.
Migration v1 creates PLAN.md's files/searches/opens/meta schema and a BLOB-keyed
`roots` table so empty queries can identify indexed roots. `PRAGMA user_version`
controls migrations; `meta.schema_version` records the same version. Unknown
schema versions are rejected. No embedding/history behavior is claimed yet.

`store_begin/put/prune/commit` form one successful root refresh. A temporary BLOB
`seen` table records visits. Upserts preserve stable AUTOINCREMENT ids; successful
scope pruning is byte-aware and distinguishes `/root` from `/root2`. Metadata
changes clear embedding columns. Commit increments catalog_gen; rollback or
connection destruction discards partial work. Callers must not prune a failed
scan. Root registration and catalog changes commit together.

A temporary `kept` table holds kept scopes: unreadable entries reported by the
crawler, plus registered roots nested in the pruned root that this scan did not
visit (e.g. an explicitly indexed hidden directory). Pruning skips everything
at or below a kept scope. An entry without stat data keeps its saved row as is.

Paths/names/extensions are always bound as BLOBs with schema type constraints.
Loaded rows reject NUL/non-BLOB paths and stream in increasing id order; callback
paths are borrowed only during the callback. Tests cover raw bytes, stable ids,
non-reuse after deletion, scope isolation, kept scopes, nested roots, rollback,
reopen durability, and callback failure propagation through integration.
Per-entry statements are prepared once per connection. The migration re-reads
`user_version` under its write lock, and opening fails if WAL cannot be enabled.
Whole-root transactions are an initial throughput baseline; M2 adds bounded
batches, async writing, reconciliation and snapshot publication.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Partial scans ADR](../../adr/0007-partial-scans-and-component-parent-matching.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
