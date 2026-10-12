# store

> **Status:** Implemented (M4; M5 history batches; M7 metadata refresh commits verified; M7 Phase 2
> signatures, mark-seen and exact prunes; M7 Phase 3 children walks and one-pass signatures):
> schema v4, versioned embedding cache plus catalog/history
> **Source:** `src/storage/store.c` · **Header:** `include/torchlight/store.h`
> **Tests:** `tests/unit/test_store.c`, `tests/unit/test_store_prune.c`, `tests/unit/test_identity.c`, CLI/daemon integration

## Behavior and ownership

`store_create` opens SQLite with WAL, foreign keys and a bounded busy timeout.
All SQL lives in this source file, is constant, and is prepared before execution.
Migration v1 creates PLAN.md's files/searches/opens/meta schema and a BLOB-keyed
`roots` table so empty queries can identify indexed roots. `PRAGMA user_version`
controls migrations; `meta.schema_version` records the same version. Migration
v2 adds a nullable 29-byte BLOB identity, preserving legacy ids and history.
Unknown schema versions are rejected. Migration v4 adds staged embedding model descriptors and prepared-text float cache; M2 writes optional
search/open history through the service writer.

`store_begin/put/prune/commit` form one refresh of one or more roots. A temporary BLOB
`seen` table records visits. Upserts preserve AUTOINCREMENT ids for unchanged
filesystem incarnations; successful scope pruning is byte-aware and distinguishes
`/root` from `/root2`. An update at the same path keeps the embedding
columns, which describe the path text (M7). Commit validates decimal catalog_gen metadata
and rejects malformed/missing values or exhaustion at INT64_MAX, then increments
it. Rollback or connection destruction discards partial work. Callers must not prune a failed
scan. Root registration and catalog changes commit together.

A prepared path lookup compares the crawler's device/inode and birth timestamp
before upserting. A changed identity retires the row and descendants even if a
replacement has the same path, size or mtime. Temporary seen/kept membership is
cleared for that scope; fresh ids and cascading open-history deletion commit
atomically. Rollback restores the old ids and history. NULL legacy identities
are adopted on the first successful scan, which cannot identify replacements
that predate the initial identity observation.

Birth timestamps preserve ids during ordinary metadata changes. Without birth
time, ctime changes conservatively retire ids/history, including descendants for
directory changes. Paired renames retain device/inode and defer adoption of the
new ctime stamp; a different object key still retires the id. The encoding and
fallback tradeoffs are in [ADR 0012](../../adr/0012-filesystem-incarnations-and-watch-fallback.md).

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

Migration v3 adds desktop_opens(event_id, desktop_id, search_id, ts) and its
retention index. Logical desktop ids do not reference files; nullable search ids
retain the existing history semantics. store_desktop_open deduplicates identical
retries and rejects conflicting events. Retention/clear delete desktop history
alongside file history in one transaction. The v1/v2 migrations preserve file
ids, identity, existing history and catalog_gen. store_load now exposes is_dir
with each entry, so clients can receive resident file/folder kinds.

## M4 integration

See [native backend/service decision](../../adr/0023-m4-native-potion-and-two-phase-search.md)
and [measured model evaluation](../../m4-model-evaluation.md).

Embedding cache operations use a separate background connection, never query SQL.
Finite little-endian floats are keyed by full emb_gen and exact prepared text.
Batches amortize WAL commits; complete staging activates and prunes cache rows
atomically. Cache changes do not advance catalog_gen or alter history.

## Scoped prunes and change sets (M6 step 3)

`store_prune_children(directory)` deletes, after a children-only rescan, the
direct children (with subtrees) the transaction did not see;
`store_prune_tree(directory)` deletes unseen descendants after a recursive
rescan. Both spare kept scopes and registered roots nested in the directory and
work in the directory's path-index range (`dir/` up to `dir0`), never a
whole-catalog scan.

A children prune used to be one `DELETE` that computed every row's direct-child
component. For a root, the range holds the whole catalog: about 0.13 s per
pass at 213k rows, the main cost of the live desktop's idle CPU (apps saving
files in the home folder). Since M7 Phase 3
([ADR 0046](../../adr/0046-children-walks-and-a-lean-ledger.md)) it walks the
range instead:
- One prepared statement steps through the rows in path order while they are
  direct children, reading whether each was seen.
- At the first row below a child, it checks whether that child was seen and
  re-seeks past the child's subtree (`child/` up to `child0`).
- Each unseen child is then deleted with its subtree, under the same rules
  that spare kept paths and nested roots.

The cost is one step per child plus one seek per child that has rows below
it. A file created in a 213k-row root now costs no measurable CPU (before:
about 125 ms per pass) and is searchable after 107 ms instead of 236 ms. Names
that sort between a child and its subtree (`a.txt` and `a-b` lie between `a`
and `a/x`) and children known only from rows below them (orphans) are handled
the same way as before. The randomized `test_store_prune.c` checks this
against a brute-force model of the old statement.

`store_track_changes(limit)` makes the connection record the ids of files rows
that catalog transactions insert, update or delete, through the existing SQLite
update hook (which also sees scoped deletions and a move's destination delete).
After commit `store_changes` borrows them sorted and deduplicated, with
`complete` false beyond the limit and `roots_changed` for root registration
changes; rollback clears them. `store_load_ids` streams the committed rows of
given ids from one read snapshot with its `catalog_gen`. Tests cover byte-prefix
siblings, kept and nested-root spares, the exact touched-id sets, load by id,
overflow and rollback.

## Metadata refreshes (M7)

Before an upsert, the prepared lookup now reads the saved row's `is_dir`
along with its identity. When the row exists, keeps its identity and is still
a file or still a directory, the upsert can only refresh mtime/size or adopt
an identity for a legacy row. The update hook then records it as a metadata
change: `store_metadata_changed` reports it, while `store_catalog_changed`
stays false and no change id is recorded. Inserts, replacements (retired and
re-inserted), file/directory kind changes, moves, prunes and root changes are
still catalog changes. The writer commits metadata-only transactions, so
later scans stop rewriting the same rows, but publishes nothing for them.
`store_commit` still advances `catalog_gen` for them, so the stored value
may run ahead of the published one until the next publication;
publications only require it to increase. Unit tests refresh metadata, adopt
an identity and check every kind of name change. The daemon regression reads
from a separate connection after reconciliation to verify the commit, intact
embedding columns, an unchanged published view and a later name publication
across the stored/published `catalog_gen` gap. A second unchanged scan does not
advance the stored `catalog_gen`. See
[ADR 0038](../../adr/0038-repair-scans-and-metadata-events.md).

## Signatures and exact scans (M7 Phase 2)

Full scans no longer send every crawled entry through SQL
([ADR 0040](../../adr/0040-cheap-full-scans.md)). The store supplies what the
writer's persisted-state map needs, and keeps deciding what may be deleted:

- **Signatures.** Every loaded row (`store_load`, `store_load_catalog`,
  `store_load_ids`) carries `signature`, a hash over exactly the columns
  `store_put` compares: `is_dir`, `mtime`, `size` and the identity bytes, each
  tagged present or absent. Since Phase 3 the tagged columns are packed into
  one buffer and hashed in one pass (`hashmap_hash_pair`); signatures live in
  memory only, so the hash may change between versions. An identity of another
  length than the 29 bytes `store_put` writes counts as absent. `store_signature(entry)` hashes a
  crawl entry the same way (false for entries without stat or identity, or
  unreadable, which must always reach `store_put`). Equal signatures at a
  path mean the upsert would change nothing. Values of an unexpected type
  count as absent, so they never match and the next scan rewrites them.
- **`store_mark_seen(path)`** marks an unchanged entry of a scoped scan seen
  without touching its row (one statement instead of three), so the scoped
  prunes, which delete what the transaction did not see, keep it.
- **`store_unseen(ids)`** declares, for an exact full scan, the rows it did
  not find (temp table `gone`). For the rest of the transaction `store_prune`
  and `store_forget_root` use variants that delete only rows in `gone`, never
  one at a path the transaction put (`seen`), with the same scope, kept-path
  and nested-root rules. A nested root counts as seen when the transaction put
  it or its row is not in `gone`. A row the caller did not know about is
  therefore kept, not deleted. `rowid IN (SELECT id FROM gone)` makes the
  cost proportional to the unseen rows instead of the catalog, and the
  `seen` table holds only the changed entries, so it no longer spills to a
  temporary file.

The membership tables (`seen`, `kept`, `gone`) are emptied outside any
transaction: after `store_commit` or `store_rollback`, and again before
`store_begin` opens the next one. They used to be emptied inside the scan
transaction, so an unchanged scan, which rolls back, restored them. After a
cold index, every later unchanged scan then deleted and journaled thousands of
rows again: about 15 MB of temp-file writes per scan at 20k entries, hidden
until exact scans removed everything else (found while validating ADR 0040).

Unit tests cover signature equality with `store_put` no-ops, `store_mark_seen`
under a scoped prune, and exact pruning. `unchanged_scans_write_nothing` in
`tests/test_daemon.py` checks, after a cold index of 6,000 files, that an
unchanged full scan writes no entry and less than 1 MiB in all (about 1.1 MB
with the old clearing). The writer's randomized equivalence
test compares exact and exhaustive catalogs end to end.

## History batches and retained opens (M5)

`store_history_write` persists one search, file open or desktop open in a
savepoint, using statements prepared once per connection. An open that
carries a query also saves its search row, in the same savepoint, so the
pair lands together or not at all. A rejected event (a deleted file, a
conflicting retry) rolls back alone. `store_history_begin` and
`store_history_commit`/`store_history_rollback` wrap many events in one
transaction: a drained queue costs one commit, and a failed commit loses the
whole batch, reported as `TL_IO`. When SQLite itself rolls back the whole
transaction during an event (an I/O error, out of memory, a trigger), the
batch ends there: that write returns `TL_IO`, the earlier events are lost,
and commit returns `TL_STATE`, so no later write commits alone under the
batch's name. `store_search`, `store_open_event` and
`store_desktop_open` keep their contracts as single-event wrappers.

`store_history_opens` streams the retained file and desktop opens since a
cutoff, oldest first, each with its search's query, in one statement. The
writer rebuilds the usage summary from it at startup. Rows the store never
writes (a negative id or time, an empty desktop id), which only another tool
could add, are skipped rather than failing the whole read. No schema change was
needed.
