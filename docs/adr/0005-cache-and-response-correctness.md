# 0005. Correct candidate caches, complete responses, and bounded snapshot lifetimes

- **Status:** Accepted
- **Date:** 2026-10-02
- **Refines:** 0003 and 0004

## Context

Further review found that narrowing from ranked top-k loses valid matches;
catalog changes and normalization can also invalidate cached membership. Counts
of unrelated lexical matches could suppress typo targets. Two-phase results
need search ids before the first response, consistent snapshots, and terminal
fallback when inference fails. Display paths and action paths need separate
fields even when filenames are valid UTF-8. Slow consumers must not block queries
or hold snapshots indefinitely.

## Decision

- Cache only complete subsequence membership, with identical matching semantics
  and `catalog_gen`; require a normalized query extension. If the bounded cache
  cannot retain complete membership, rescan rather than narrow from top-k.
- Masks must conservatively handle every symbol. Lookup deletion neighbours for
  eligible complete tokens regardless of unrelated match counts, then verify
  edit distance. An exact full-basename hit may skip typo lookup.
- Assign search ids before responding, enqueue history once, and pin the same
  snapshot pair for lexical and final phases. Semantic failure/deadline finishes
  with lexical fallback. Cancellation is per client; running inference need not
  support preemption. M2 is lexical-only; M4 adds semantic two-phase execution.
- Return exact `path` or `path_b64` alongside display, with string file ids.
  Resolve ids against the current catalog before launch, preserve selection by
  id, and deduplicate launch history by event id. Provide CLI NUL-delimited output.
- Prepare index deltas before commit, handle post-commit publication failure via
  reload/rebuild, and protect snapshot acquisition/reclamation. Bound clients,
  scratch, caches, inference and nonblocking output queues.
- Preloading avoids explicit file reads, not all OS paging. Budget total peak
  memory as well as vector payload. Evaluate reduction against full-dimensional
  and labeled quality; version identical query/path transforms. A binary-only
  shortlist cannot improve precision without more accurate rescoring.
- Define multiword/empty queries, versioned Unicode normalization with opaque
  invalid-byte symbols, own-state exclusion, hidden allowlist traversal, and
  configuration/retention reconciliation.

## Alternatives considered

- Cache only returned results: loses later matches that were previously below top-k.
- Gate typos by result count: irrelevant subsequences can conceal intended files.
- Wait for successful inference only: CLI clients may never receive completion.
- Atomic pointer load followed by reference increment without lifecycle protection:
  can race reclamation.
- Treat preloaded heap memory as permanently resident: OS paging still applies.

## Consequences

More explicit contracts make regression cases and resource limits implementable.
The architecture and milestone order remain, with benchmark uncertainty stated
as a gate rather than a guaranteed result. No code or dependencies are introduced.
