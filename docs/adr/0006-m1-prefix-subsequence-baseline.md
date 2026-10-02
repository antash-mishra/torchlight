# 0006. First M1 prefix/subsequence baseline

- **Status:** Accepted; the unreadable-scope rollback rule is refined by 0007
- **Date:** 2026-10-02
- **Refines:** 0003, 0004 and 0005

## Context

The repository began with planning documents only. M1 explicitly calls for
small benchmarked increments: prefix/subsequence first, then trigram and typo.
A runnable local CLI needs durable raw paths and consistent Unicode matching.
SQLite and utf8proc were explicitly approved by the user before addition.

## Decision

- Deliver build tooling, checked utilities, XDG defaults, physical crawling,
  transactional SQLite persistence, Unicode tokenization and a reusable sealed
  prefix/subsequence engine in the first increment. Keep M1 marked in progress.
- Normalize valid grapheme clusters with utf8proc NFC/case-folding, retaining
  source cluster offsets/boundaries. Keep invalid bytes as distinct symbols;
  derive display independently from exact action bytes.
- Sort borrowed basename/token keys and owned initials. Scan subsequences fully
  and score greedy boundary/consecutive matches. Keep exact raw path priority
  separate. Cache no top-k membership. Treat strict parent-token matching,
  optimal alignment, trigram and bounded edit retrieval as subsequent work.
- Preallocate engine-bound query scratch and use bounded deterministic top-k.
  Pure stateless subsequence/scoring helpers need no artificial opaque objects.
  The initial engine owns entries until destroy; M2 introduces pinned immutable
  catalog_gen lifetimes and background publication.
- Refresh one root per transaction. Preserve ids, record complete seen membership,
  and prune only a successful scope. Roll back the whole root on unreadability.
  Store root membership in an added `roots` table in the first schema migration.
- Benchmark the release engine independently of SQLite and CLI startup at 50k
  and 500k synthetic paths. Record latency/RSS without declaring targets met.

## Alternatives considered

- Deliver all lexical channels before measuring: conflicts with incremental M1.
- Bootstrap ASCII-only normalization: avoided after utf8proc approval.
- Persist display strings or replacement characters: loses exact byte identity.
- Narrow using returned results: loses matches outside the previous top-k.
- Prune partial scans: mistakes inaccessible paths for deletions.
- Add daemon concurrency now: belongs to M2 and needs explicit snapshot ownership.

## Consequences

There is an end-to-end local implementation with sanitizer/regression coverage,
while full M1 retrieval quality and p95 latency remain open. The full-scan baseline
exposes memory and traversal costs for later improvements. Configuration files,
hidden allowlists, batching/cached statements, and richer ranking remain visible
follow-ups. Existing design targets and later milestone order remain unchanged.
