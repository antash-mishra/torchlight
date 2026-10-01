# lexical

> **Status:** M1 in progress: prefix/subsequence engine implemented
> **Source:** `src/index/lexical.c` · **Header:** `include/torchlight/lexical.h`
> **Tests:** `tests/unit/test_lexical.c`

## Behavior and ownership

The opaque engine copies exact paths and owns normalized entries plus its
prefix channel. Add monotonically increasing nonzero ids before sealing; a
channel-build failure poisons the builder. The caller destroys an engine only
when no workspaces/readers remain. M2 snapshot lifecycle/publication is planned.

Each workspace is bound to one sealed engine and allocates two per-entry score
arrays once. Queries normalize up to 256 bytes into fixed scratch, require every
whitespace-separated word to match, and combine prefix/subsequence scores.
Basename matches beat parent context, and exact normalized basenames/raw paths
have explicit priority. A word that misses the basename must match within one
parent directory name (the nearest matching one), so letters scattered over a
long absolute path no longer match; a word containing `/` may match across the
full path. Entries rejected by an earlier word are skipped for later words.
Token-level parent matching and root-prefix exclusion remain M1 follow-ups. Space-only
queries list indexed roots. Results borrow exact bytes and carry stable ids.

Every entry is considered before bounded top-k insertion. Ties sort by raw path
bytes and then id. Queries perform no SQL, filesystem I/O or heap allocation.
There is no membership cache and therefore no top-k narrowing. Warm/cold tests
cover appends, edits, backspaces, multiword queries, Unicode, raw-byte identity,
limits, exact priorities and workspace reuse. Trigram, typo and complete-cache
narrowing remain planned. See evaluation for baseline latency/memory limits.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Component-scoped parent matching ADR](../../adr/0007-partial-scans-and-component-parent-matching.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
