# 0009. Unresolved root identities and refresh failures

- **Status:** Accepted
- **Date:** 2026-10-02
- **Refines:** 0008

## Context

Configuration sync canonicalized available roots with `realpath` but kept
unavailable absolute paths as written. A root spelled with a trailing slash,
dot components or a symlink could then stop matching its registered canonical
path. The sync forgot its catalog entries despite reporting that they were kept.

`crawl_run` propagates callback errors and also returns `TL_IO` for filesystem
failures. The CLI interpreted every `TL_IO` as an unavailable root, including
SQLite write failures. With another root available, the run committed and
reported success despite the storage error.

## Decision

- Track whether any selected root's canonicalization failed. During configuration
  sync, keep unmatched registered roots instead of forgetting them when their
  identity is uncertain. Add their canonical scopes to `store_keep` before
  pruning scanned roots, so an ancestor scan cannot delete them either.
- Report deferred removals and retry them on the next sync where all configured
  root identities resolve. Continue refreshing available roots. A run with no
  successful root scan still rolls back and fails.
- Record the save callback's status in its caller-owned context. A storage error
  aborts the refresh and rolls back every root's writes, registration changes and
  `catalog_gen`. Only filesystem failures get the unavailable-root fallback.
- Treat `realpath` allocation failure as `TL_NOMEM`, not an unavailable path.

## Alternatives considered

- Strip trailing slashes only: misses dot components and unavailable symlink
  aliases. Lexical normalization cannot recover a missing symlink's target.
- Persist configured spellings and canonical root identities: a future schema
  migration could enable more precise removal during outages, but is unnecessary
  for safe M1 refreshes.
- Fail every sync containing an unavailable root: prevents useful refreshes of
  accessible roots.
- Give callback failures a new status code: loses their original error and is
  unnecessary when the caller can record their source in its callback context.

## Consequences

Unavailable configured roots no longer establish deletion, even through an
alias or a scanned ancestor. Unrelated removed roots can stay registered until
all configured roots resolve; this is intentional conservative behavior and is
reported to the user. Storage failures return an error and preserve the previous
catalog exactly. No schema, dependency or lexical-query behavior changes.

CLI regressions exercise trailing slashes, dot components, symlink aliases,
ancestor scans and recovery. Test-only SQLite triggers inject a write failure
with the failing root both before and after a healthy root; catalog snapshots
verify IDs, metadata, root registration and `catalog_gen` remain unchanged.
