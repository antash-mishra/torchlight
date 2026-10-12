# crawl

> **Status:** Implemented (M1/M2; M6 scoped walks; M7 device boundaries; M7 Phase 2 one stat per
> entry): physical crawl, allowlist, roots and filesystem identity
> **Source:** `src/fs/crawl.c` · **Header:** `include/torchlight/crawl.h`
> **Tests:** `tests/unit/test_crawl.c`, `tests/test_cli.py`

## Behavior and ownership

The opaque crawler copies an excluded state scope and an allowlist, and walks
with fts using FTS_PHYSICAL|FTS_NOCHDIR. Roots are canonicalized, symlink entries
are reported, and directory symlinks are never traversed. A callback borrows each
path/stat record; callback errors propagate and walking closes its resources.

Stat records now include device/inode and a birth timestamp where `statx`
supports it. A fresh statx observation supplies both identity and metadata,
without following symlinks. Missing birth time falls back to ctime; unsupported
statx falls back to one `fstatat`. Other lookup failures mark the scope unreadable so the
store cannot mistake failed observation for replacement. The store decides
whether a changed incarnation must retire an id; crawl never calls storage.

**One stat per entry (M7 Phase 2).** The walk runs with `FTS_NOSTAT`, so fts
no longer stats entries whose `d_type` says they are not directories, and the
single `statx` in `file_identity` is the only stat of a file (213k fewer
`newfstatat` calls per scan of the home mirror). Under `FTS_NOSTAT` glibc
leaves `fts_statp` NULL for every entry. With `FTS_NOSTAT|FTS_PHYSICAL` it
also guesses a directory's subdirectory count from its link count and stops
entering subdirectories once the guess is used up. On a filesystem that
reports 2 links for every directory (some FUSE filesystems), subtrees would
vanish silently, which is why `find` has `-noleaf`. The walk therefore sets
each directory's `fts_nlink` to 1 before fts lists it, which turns the guess
off (checked on glibc 2.39: with a forced count of 2, subdirectories became
unstat'd files). An unstat'd entry that turns out to be a directory (replaced
since the listing) is reported unreadable, so its saved subtree is kept and
repaired. `one_stat_records` in `test_crawl.c` compares every record of a
root walk and a scoped walk with `lstat`. See
[ADR 0040](../../adr/0040-cheap-full-scans.md).

**Device boundaries (M7).** A directory record sets `device_boundary` when it
is the walk's starting directory or lies on a different device than its parent
(a mount point), compared through `fts_dev`, which fts keeps for directories
even under `FTS_NOSTAT`. The writer checks only those directories' filesystem types,
one `statfs` each, to find network and FUSE mounts that need periodic repair.
`device_boundaries` in `test_crawl.c` covers a root walk and a scoped walk.

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
recovery are implemented by the M2 writer. Scan cost is proportional to visited
entries; storage policy stays outside crawl.

## Related

- [Partial scans ADR](../../adr/0007-partial-scans-and-component-parent-matching.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- [Filesystem identity ADR](../../adr/0012-filesystem-incarnations-and-watch-fallback.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.

## Scoped walks (M6 step 3)

`crawl_scope(crawler, directory, recursive, ...)` rescans one directory that a
root's walk indexes: it reports the directory itself (with `is_root` false),
its direct children (child directories are reported but not entered), or with
`recursive` its whole subtree, using exactly the root walk's exclusion,
allowlist and unreadable-scope rules below it. A missing or unreadable
directory returns `TL_IO` so the caller prunes nothing. The writer uses it for
watch-event scopes; see [writer](../writer/README.md).
