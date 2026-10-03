# 0013: Query contracts and complete resident filtering

**Status:** Accepted

## Context

The M1/M2 readiness review reproduced two query-contract violations. Trigram
deduplication called libc `qsort`, which allocated scratch for accepted words of
131 bytes and longer on glibc. Typo lookup accepted two- and 33-symbol query
words despite the documented 3–32-symbol range. At 500k paths, full and parent
context scans also exceeded the 5 ms p95 target.

Profiling showed repeated random ancestor walks and visits to entries whose
basename and individual parent names could not match. The combined context mask
admitted symbols scattered across different ancestors. That filter remained
conservative but did unnecessary work.

## Decision

Use the reusable core in-place integer heapsort for trigram keys and enforce
the documented inclusive typo-query bounds. Add a separate glibc allocator
interposition check to `make test`, covering long queries and active workers.

Index basename masks as immutable resident bitmaps. A non-path scan intersects
the required bits, then unions channel hits and entries whose nearest usable
parent has a positive score. Resolve large batches' directories sequentially
in parent-before-child order. The set is complete; mask collisions add work
and cannot lose results. Complete subsequence membership remains the only
input to incremental narrowing.
Keep four bounded word evidence caches within a query, so channel-first
evaluation and fallback can reuse complete channel hits and parent scores.
Query epochs invalidate them when word buffers change; eviction changes work,
never results.

Choose the first multiword scan using basename-mask counts, possible parent
descendants and complete cached membership sizes. Estimates affect order only.
Evaluate first-word channel hits first and skip the remaining scan only when
the heap strictly beats a proven upper bound for every unhit entry. Bounds
use observed channel maxima when available. Seed exact multiword basename
matches independently so words containing internal separators retain priority.

For engines with at least 65,536 entries, preallocate a pool with the caller
and three workers. Batches of at least 4,096 entries score disjoint ranges
against fully resolved, read-only directory context. The caller records
membership, compacts candidates and orders results. `/` words remain serial
because their reconstruction buffer is workspace-owned mutable scratch.
The existing pthread dependency supplies the pool; no dependency is added.

## Consequences

No candidate budget or file-id truncation is introduced. Scores, exact-match
priority and deterministic ties are preserved. Mask postings use approximately
8 bytes per entry; directory lists/estimates use 4 bytes per entry and 16 bytes
per directory. Compared with one word's original scratch, four evidence caches
add 24 bytes per entry and 36 bytes per directory. The batch adds 8 bytes per
entry and bitmap scratch adds one bit per entry, plus pthread stacks. Large
reader counts also multiply worker resources, so the daemon's default one-reader
bound remains appropriate. Creation may return `TL_IO` if workers cannot be started; cleanup
joins every successfully started worker.

Scalar bitmap checks, analytic large-corpus ranks, capacity/cache regressions,
ASan/UBSan and allocator interposition validate the changes. A differential
run over 6,026 typing/backspace cases at 50k and 6,082 at 500k paths matched the
previous query algorithm with the same typo-bound fix. Release measurements and
remaining limits are recorded in [evaluation](../evaluation.md). The user accepts roughly
6 ms p95 for advancing to M3; the original 5 ms optimization target is deferred
until the whole system is built.
