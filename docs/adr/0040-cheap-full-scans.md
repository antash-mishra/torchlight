# 0040. Cheap full scans: one stat, one long-lived watcher, a persisted-state map

- **Status:** Accepted
- **Date:** 2026-10-11
- **Supersedes:** 0012's watcher rule "replace it only after successfully
  building a new one" (build-then-swap on every full scan)
- **Amended by:** [0046](0046-children-walks-and-a-lean-ledger.md) (one 128-bit
  fingerprint instead of two FNV-1a hashes, 4-byte index slots, bulk refills)

## Context
After M7 Phase 1 a full scan of every root still runs at startup, after
inotify overflow, for `reconcile`, as the hourly backstop, after a relevant
mount change and every `rescan_ms` while coverage is degraded. On the 213k
home mirror one that found nothing changed cost 3.9 s and 4.1 CPU-s, and
13.2 s at 494k ([M7 plan](../m7-plan.md)). Almost all of it was waste:

- `fts` stat'd every entry, then `file_identity` ran `statx` on it again.
- Every entry went through SQLite: an identity `SELECT`, an upsert attempt and
  an `INSERT INTO seen`, about 640k statements, and the `seen` temp table
  spilled about 100 MiB to a temporary file, read back by
  `DELETE … NOT IN seen`.
- A new inotify instance was built, re-adding all 34k watches (68,942 kernel
  watches until the swap, against a per-user limit shared with editors).

## Decision

**One stat per entry.** The walk runs with `FTS_NOSTAT`; `file_identity` takes
every field from its one `statx`, with an `fstatat` fallback where `statx` is
unavailable. Under `FTS_NOSTAT` glibc leaves `fts_statp` NULL for every entry,
so the mount-point check uses `fts_dev`, which fts keeps for directories.
glibc's `fts` also guesses subdirectory counts from directory link counts
under `FTS_NOSTAT` and stops entering subdirectories once the guess is used
up. Filesystems that misreport link counts would lose whole subtrees (the
reason `find -noleaf` exists). The walk sets each directory's `fts_nlink` to
1 before fts lists it, which turns the guess off, so only entries whose
`d_type` says "not a directory" skip the stat. An unstat'd entry that turns
out to be a directory (replaced since the listing) is reported unreadable, so
its saved subtree is kept and repaired.

**One long-lived watcher.** Full scans add into the live watcher, as scoped
scans did. `watch_begin` starts a scan generation. `watch_add(path,
identity)` makes no system call when the slot at that path holds the same
object (device and inode from the crawl). A path match alone is not enough:
a directory moved out of every root keeps its watch under the stale path,
and a new directory there must get its own. Otherwise `inotify_add_watch`
runs. An object already watched under another path adopts the new path.
After a successful full scan, `watch_sweep` removes the watches nothing
confirmed (deleted, excluded or unrooted directories), sparing those below
roots the scan could not read. Directory moves confirm the slots they
relocate. The `unavailable` counter now counts failures since the last
`watch_begin`, so `watch_degraded` describes current coverage, as a fresh
watcher did. A new instance is created only when none exists.

**Compare in memory, write only differences.** A new module, the ledger
(persisted-state map), holds every committed row as its id, two independent
64-bit FNV-1a hashes of its path (a hash-index key and a check) and the store
signature. The signature is a hash of the columns `store_put` compares (is_dir,
mtime, size, identity bytes), computed by `store_signature` for crawl entries
and returned with every loaded row, so equal signatures mean `store_put` would
change nothing. Paths are not stored. Passes that can use a complete ledger
are *exact*:

- A full exact pass leaves unchanged entries out of the batch, so it also
  skips the path copies. It sends new, changed and uncomparable entries
  (unreadable, no stat) through `store_put`. `store_unseen` declares the rows
  it did not find (a `gone` temp table), and `store_prune` and
  `store_forget_root` then delete only among them, and never a row at a path
  the transaction put. Scope, kept-path and nested-root rules are unchanged.
  The cost is proportional to the unseen rows (a rowid `IN` list), not the
  catalog.
- A scoped exact pass sends unchanged entries as `store_mark_seen` (one
  statement instead of three), because its prunes still delete what the
  transaction did not see.
- After each exact commit the writer reloads into the ledger every row the
  commit touched (`store_changes`) or rewrote (rows found changed), read on
  the reader connection. When the touched rows are unknown (tracking
  overflowed, or roots changed), the ledger is marked incomplete. A full
  build (startup, recovery, compaction) refills an incomplete ledger from the
  same rows it loads, with no extra SQL. A full pass that finds it incomplete
  reloads it first. A scoped pass then simply writes everything.
- A replaced directory is the one case where skipping unchanged entries
  changes the outcome. When a different object now holds a directory's path,
  `store_put` retires the saved subtree, which the old behavior wrote back in
  the same batch. An exact pass remembers every directory it found changed.
  If the commit removed that row, the writer schedules a recursive rescan of
  the path, which writes the descendants back (with new ids, as before).
- `tl_writer_options.exhaustive` (testing only) disables the ledger, keeping
  the previous behavior for the randomized equivalence test.

`last_scan_writes` and `last_scan_unseen` in status show what the last pass
sent to the store.

## Alternatives considered
- **Verify path hashes against the published snapshot** (the plan's
  proposal): saves 8 bytes per row but couples scan correctness to which
  snapshot is published, including the states where publication failed or
  only metadata was committed. Two hashes keep the ledger self-contained.
  A collision could at worst keep a vanished row (a stale result), because
  deletion only touches declared rows at paths the transaction did not put.
- **Store paths in the ledger**: exact without hashing, but about 20 MiB more
  at 213k entries, duplicating the lexical engine.
- **Load the ledger from SQLite at the start of every full scan**: no
  resident memory, but it adds the 0.33 s catalog read to every scan, missing
  the 1.2 CPU-s target. Startup already reads every row, so it fills the
  ledger then.
- **Predict the store's replacement rule in memory** and write a replaced
  directory's descendants in the same batch: needs the identity bytes or their
  kind in every record, and must match `identity_differs` exactly (kind 2
  renamed rows, ctime fallback). Asking the catalog afterwards is exact by
  construction.
- **SQLite tuning** (`temp_store=MEMORY`, `mmap_size`): 5 to 13% faster for
  200 to 400 MB more RSS ([plan](../m7-plan.md#storage-alternatives-considered-sqlite-tuning-and-mmap)).
- **Skip unchanged directories** (Step 2.4, `mlocate` style): decided against
  for now; see the plan's results.

## Consequences
- **Memory.** The ledger costs about 32 bytes per row plus its hash index.
  The index allows at most 50% load: 12.6 MiB at 213k entries (62 bytes each)
  and 27.2 MiB at 494k, more than the plan's 24-byte estimate. The full scan no longer
  copies every path into its batch.
- **Membership tables.** `seen`, `kept` and `gone` are now emptied outside
  any transaction. Emptied inside a scan transaction, as before, they came
  back with every rollback, so after a cold index each unchanged scan
  re-deleted them: about 15 MB of temp-file writes per scan at 20k entries.
  Exact scans exposed this once everything else was gone. A daemon test now
  bounds the bytes an unchanged scan writes.
- **Failure modes.** A failed refresh or reload marks the ledger incomplete,
  and passes fall back to the previous exhaustive behavior until a full build
  or full pass reloads it, so a ledger fault costs speed, never rows.
- **Replaced directories.** After a commit that replaced a directory, its
  unchanged descendants are missing from the catalog until the follow-up
  scoped pass (about 0.1 s). On filesystems without birth time, every changed
  directory counts as replaced (ADR 0012's conservative ctime fallback). There
  each such directory costs one follow-up rescan of its subtree, which the old
  behavior paid inside the same pass.
- **Watcher.** Kernel watches never exceed the steady count during a full
  scan. Directories excluded by a configuration change lose their watches at
  the next full scan instead of with the replaced instance.
- **Tests.**
  - The randomized equivalence test runs exact and exhaustive writers side by
    side over random creates, deletes, renames (inside and out of the tree),
    replacements, touches, appends, symlinks and unreadable directories,
    offline and live, and requires identical catalogs.
  - A deterministic replaced-directory case fails when the follow-up rescan
    is disabled.
  - Watch tests cover the skipped system call (provable with an unreadable
    directory), sweeping and stale paths.
  - Crawl tests check every record's metadata against `lstat`.
