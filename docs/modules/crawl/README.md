# crawl

> **Status:** M1 physical crawl implemented; allowlists/reconciliation planned
> **Source:** `src/fs/crawl.c` · **Header:** `include/torchlight/crawl.h`
> **Tests:** `tests/unit/test_crawl.c`

## Behavior and ownership

The opaque crawler copies an optional excluded scope and uses fts with
FTS_PHYSICAL|FTS_NOCHDIR. Roots are canonicalized, symlink entries are reported,
and directory symlinks are never traversed. A callback borrows each path/stat
record; callback errors propagate and walking closes its resources.

Hidden directories and node_modules/target/build/__pycache__ are intentionally
excluded. Hidden files are retained; explicit roots override name defaults.
An excluded state scope always wins. The CLI also omits custom DB/WAL/SHM files.
Below the root, unreadable directories and failed stats are reported with
`unreadable` set and the walk continues; the store then keeps their saved
entries instead of pruning them. fts reports an unreadable directory twice:
normally, then as unreadable once listing it fails. A missing/unreadable root or
a walk error returns TL_IO, and the CLI rolls back the whole root.
Intentionally excluded unreadable hidden scopes remain exclusions.

Unit tests isolate invalid roots and callback propagation; CLI fixtures cover
symlink cycles, hidden files/directories, explicit hidden roots, state exclusion,
raw bytes, removal, unreadable-scope preservation and continued scanning. Hidden-descendant allowlists,
multiple-root canonical deduplication, watches and periodic recovery are planned.
Scan cost is proportional to visited entries; storage policy stays outside crawl.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Partial scans ADR](../../adr/0007-partial-scans-and-component-parent-matching.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
