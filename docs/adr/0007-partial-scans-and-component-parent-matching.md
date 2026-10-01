# 0007. Partial scans, nested roots and component-scoped parent matching

- **Status:** Accepted
- **Date:** 2026-10-02
- **Refines:** 0006

## Context

A review of the first M1 increment found three problems:

1. Any unreadable directory or failed stat below a root failed the whole scan
   and rolled it back. A real `$HOME` almost always contains such a path, so
   indexing it would never succeed.
2. Re-indexing a root pruned every unseen entry in its scope, including an
   explicitly indexed hidden root nested inside it (the parent's crawl skips
   hidden directories by default).
3. When a word missed the basename, its subsequence was matched across the full
   absolute path. Letters scattered over `/home/user/...` made short queries
   match nearly every entry.

## Decision

- The crawler continues past unreadable directories and failed stats below the
  root and reports them with `unreadable` set. A missing or unreadable root
  still fails, so nothing is pruned when nothing was scanned.
- The store keeps every saved entry at or below an unreadable path for that
  scan (temporary `kept` scopes). An entry without stat data keeps its saved
  row unchanged.
- Pruning a root also keeps registered roots nested inside it that the scan
  did not visit. Nested roots that the scan did visit are pruned normally.
- A query word that misses the basename must match within a single parent
  directory name, taking the nearest matching one. A word containing `/`
  expresses path structure and may still match across the full path.

## Alternatives considered

- Keep all-or-nothing scans: safe, but unusable on real home directories.
- Treat unreadable subtrees as deleted: violates PLAN.md's rule that an
  unreadable scope is never mistaken for deletion.
- Protect every nested root unconditionally: hides deletions under visible
  nested roots until they are re-indexed themselves.
- Best (maximum) component score instead of nearest match: measured about 50%
  slower at p50 on the 500k benchmark; parent token prefixes already rank
  strong parent matches higher.
- Exclude the indexed root prefix from parent matching: needs root awareness in
  `lexical`; left for the strict parent-token work.

## Consequences

Indexing a home folder survives permission errors and concurrent deletions.
Stale entries under a permanently unreadable path, or under a nested root
whose directory has been removed, stay until that scope is scanned again or
root removal is added with config support. Parent matching gives far fewer
spurious results. Warm p50 at 500k paths rose by roughly 10ms because of the
per-component scoring; the p95 target remains unmet as before.
