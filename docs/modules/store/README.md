# store

> **Status:** Implemented (M1/M2): catalog/roots, coherent loads, pending views, byte renames and history
> **Source:** `src/storage/store.c` · **Header:** `include/torchlight/store.h`
> **Tests:** `tests/unit/test_store.c`

## Behavior and ownership

`store_create` opens SQLite with WAL, foreign keys and a bounded busy timeout.
All SQL lives in this source file, is constant, and is prepared before execution.
Migration v1 creates PLAN.md's files/searches/opens/meta schema and a BLOB-keyed
`roots` table so empty queries can identify indexed roots. `PRAGMA user_version`
controls migrations; `meta.schema_version` records the same version. Unknown
schema versions are rejected. Embedding search remains planned; M2 writes optional
search/open history through the service writer. No schema change was needed.

`store_begin/put/prune/commit` form one refresh of one or more roots. A temporary BLOB
`seen` table records visits. Upserts preserve stable AUTOINCREMENT ids; successful
scope pruning is byte-aware and distinguishes `/root` from `/root2`. Metadata
changes clear embedding columns. Commit validates decimal catalog_gen metadata
and rejects malformed/missing values or exhaustion at INT64_MAX, then increments
it. Rollback or connection destruction discards partial work. Callers must not prune a failed
scan. Root registration and catalog changes commit together.

A temporary `kept` table holds kept scopes: unreadable entries reported by the
crawler, plus registered roots nested in the pruned root that this scan did not
visit (e.g. an explicitly indexed hidden directory). Pruning skips everything
at or below a kept scope. An entry without stat data keeps its saved row as is.
`store_keep` adds a kept scope directly (e.g. a configured root that is
currently unavailable).

`store_roots` streams registered roots. `store_forget_root` unregisters a root
that left the configuration and deletes its saved entries that no scan in the
same transaction saw or kept. Prune and forget share one deletion statement.
Forgetting runs before pruning, so a forgotten root nested in a scanned root is
no longer protected as an unvisited nested root.

Paths/names/extensions are always bound as BLOBs with schema type constraints.
Loaded rows reject NUL/non-BLOB paths and stream in increasing id order; callback
paths are borrowed only during the callback. Tests cover raw bytes, stable ids,
non-reuse after deletion, scope isolation, kept scopes, nested roots, forgotten
roots, rollback,
reopen durability, and callback failure propagation through integration.
Per-entry statements are prepared once per connection. The migration re-reads
`user_version` under its write lock, and opening fails if WAL cannot be enabled.
`store_load_catalog` streams rows and persisted catalog_gen under one read
transaction, so another WAL connection's commit cannot mislabel the resident
view. It refuses an active scan and rolls back callback failures; callers must
discard partial builder output then. Tests commit concurrently during loading
and verify that the subsequent load alone sees the new rows and catalog_gen.

`store_prepare_catalog` streams pending transaction rows with the next
`catalog_gen`, so candidate construction can fail before commit. A per-connection
change hook tracks files/roots, excluding temporary membership and sequence
bookkeeping; unchanged daemon scans roll back without rebuilding or advancing
`catalog_gen`. Transactions are bounded by the writer's entry/path-byte limits.

`store_move` preserves ids for exact paths and descendants, replaces destination
rows, refreshes the moved basename/extension, and clears path embeddings. SQLite
concatenation is explicitly cast back to BLOB. Late moves also remap temporary
seen/kept membership before pruning. Rename tests cover files and directory trees.

History APIs persist idempotent searches and unique launch events independently
of catalog transactions. Missing retained searches become NULL; missing file ids
and conflicting retries return TL_STATE. Retention/clear transactions affect both
history tables and never catalog_gen. The async writer preserves FIFO ordering;
integration tests verify disabled history, deduplication and clear.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- [Partial scans ADR](../../adr/0007-partial-scans-and-component-parent-matching.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
