# crawl

> **Status:** Implemented (M1): physical crawl, allowlist, root coverage; watches/reconciliation planned
> **Source:** `src/fs/crawl.c` · **Header:** `include/torchlight/crawl.h`
> **Tests:** `tests/unit/test_crawl.c`, `tests/test_cli.py`

## Behavior and ownership

The opaque crawler copies an excluded state scope and an allowlist, and walks
with fts using FTS_PHYSICAL|FTS_NOCHDIR. Roots are canonicalized, symlink entries
are reported, and directory symlinks are never traversed. A callback borrows each
path/stat record; callback errors propagate and walking closes its resources.

**Scopes.** One decision function (`classify`) is shared by walks and coverage
checks:

- Hidden directories and `node_modules`/`target`/`build`/`__pycache__` are
  skipped; hidden files in visible directories are kept; the root is always
  walked.
- An allowlisted directory is indexed like any other, and the defaults apply
  again below it.
- Hidden or ignored ancestors of an allowlisted directory are *traversed*: not
  reported, and below them the walk follows only paths that lead to an
  allowlisted directory (`allow = ~/.config/nvim` indexes `nvim`, not the rest of
  `~/.config`).
- The excluded state scope always wins, even below roots or allowlisted
  directories.

**Unreadable scopes.** Below the root, unreadable directories and failed stats
are reported with `unreadable` set and the walk continues; the store keeps their
saved entries. fts reports an unreadable directory twice (normally, then as
unreadable). An unreadable traversed directory is reported without stat data, so
it protects saved entries without becoming a catalog row. A missing/unreadable
root or a walk error returns TL_IO. Intentionally excluded unreadable scopes
remain exclusions.

**Roots.** `crawl_covers(outer, inner)` says whether a crawl of `outer` would
index directory `inner` itself; `crawl_select_roots` drops repeated roots and
roots covered by another one, so overlapping configuration scans each path once.
A root inside a hidden directory is not covered and stays separate.

Unit tests cover invalid roots, callback propagation, unreadable directories,
allowlist traversal (indexed, traversed and skipped paths) and coverage/selection.
CLI fixtures cover symlink cycles, hidden files/directories, explicit hidden
roots, state exclusion, raw bytes and configured allowlists. Watches and periodic
recovery are planned for M2. Scan cost is proportional to visited entries;
storage policy stays outside crawl.

## Related

- [Partial scans ADR](../../adr/0007-partial-scans-and-component-parent-matching.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
