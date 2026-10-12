# 0046. Children prunes by index walk, and a lean ledger

- **Status:** Accepted
- **Date:** 2026-10-12
- **Amends:** [0040](0040-cheap-full-scans.md) (the ledger's fingerprint, index
  layout and memory figures)

## Context
Two costs were left after M7 Phase 2
([health report](../reports/m7-phase2-health-report.html)):

- **Rescans of a root folder.** On the live desktop, background CPU was 0.34%
  of a core against 0.25% in the Phase 1 review. Apps that save files directly
  in the home folder trigger a children rescan of the root. Its cleanup,
  `PRUNE_CHILDREN_SQL`, deleted every row in the root's path range whose
  direct-child component was not seen, so it evaluated an expression for every
  row of the catalog. On the 213k mirror a file created in the root was
  searchable after 236 ms (108 ms elsewhere) and cost 250 ms of CPU per
  create/delete pair; 2.43 of 2.51 CPU-s on the persistence thread were in
  that statement.
- **The ledger.** Filling it made search ready 0.2 s later after a restart
  (1,524 against 1,325 ms before Phase 2). Each row cost two byte-by-byte
  FNV-1a passes over its path, a 12-byte slot in an index that grew 13 times
  during the load, and a five-call FNV signature. The same hashing ran for
  every entry of a full scan (5.7% of the indexing thread). The ledger was
  12.6 MiB at 213k, against an estimate of 5 MiB.

## Decision

**Walk a folder's children instead of its rows.**
- `store_prune_children` steps through the directory's range in path order
  with one prepared statement while rows are direct children, reading whether
  each was seen.
- At the first row below a child, it checks whether that child was seen and
  re-seeks to `child0`, past the child's subtree (`child/...`).
- It then deletes each unseen child and its subtree with the old rules that
  spare kept paths and nested roots.

Every child is examined, with its row or (an orphan) through its first
descendant. Every row below an unseen child goes, and nothing else. So the
deleted set is the one the old statement deleted, at a cost of one step per
child plus one seek per child that has rows below it. Names that sort between
a child and its subtree (`a.txt`, `a-b` between `a` and `a/x`) are ordinary
children in the walk.

**One hash pass, 4-byte slots and bulk refills.**
- `key` and `check` are the two halves of `hashmap_hash_pair`, MurmurHash3
  x64_128 with seed 0, verified against the reference vectors.
- Store signatures pack their tagged columns into one buffer and take one
  half of the same hash.
- Both values exist in memory only, so changing them needs no migration.
- The core hashmap gains an external-hash mode in which slots hold only the
  record position and the map reads a slot's hash from the record
  (`hashmap_create_external`), and `hashmap_reserve`.
- During a refill (`ledger_reset` to `ledger_seal`) puts only append records,
  and the seal builds the index once at its final size.

## Alternatives considered
- **A parent column with an index** (the report's first suggestion): direct
  children by one index lookup, but a schema migration, an index of about
  213k entries on disk, and parent maintenance in moves and replacements. The
  walk needs only the existing path index.
- **Compare a folder's children in memory**: the ledger stores no paths, and
  the published snapshot is not the catalog of record (ADR 0040 rejected that
  coupling).
- **Skip-seek per child even for leaf children**: a folder with many leaf
  files would pay a seek per file. The walk steps through leaves and seeks only
  over subtrees.
- **A sorted array instead of the hash index**: about 18 probes per lookup
  (estimated 50 to 100 ms more per full scan, not measured). 4-byte slots save
  the same memory.
- **Keep FNV-1a, but compute both hashes in one loop**: estimated to halve the
  hashing cost; eight-byte words cut it to about a quarter (the profile's
  hashing share fell from 5.7% to 1.1% of the indexing thread).
- **Open the socket before the catalog loads**: queries during the load
  would get empty or partial answers. Making the load cheaper keeps every
  answer complete.

## Consequences
- 213k mirror, `-O3`, alternating with HEAD (`97deb1d`):
  - A file created or deleted in the root is searchable after 107 ms
    (was 236 to 243 ms), and a create/delete pair costs 0 to 10 ms of CPU
    (was 250 to 260 ms).
  - Search is ready after a restart in 1,418 ms (was 1,527; 1,386 before
    Phase 2). The startup scan takes 1,043 ms (was 1,094).
  - A no-change full check takes 943 to 969 ms and 0.97 CPU-s (was 993 to
    1,021 ms and 1.03 CPU-s).
  - The ledger is 8.5 MiB (was 12.6) and RSS 4 to 5 MiB lower.
- The busiest folder (2,838 entries) is unchanged at about 140 ms: its cost is
  the scan and `store_mark_seen` per child, not the prune.
- The 128-bit fingerprint reads words in native byte order. It must never be
  persisted or compared across machines.
- Tests: `test_store_prune.c` compares the walk with a brute-force model of
  the old statement over random trees (four mutations of the walk fail it, a
  fifth hangs). `test_core.c` checks external-hash maps and the reference
  vectors, and `test_ledger.c` checks refills.
