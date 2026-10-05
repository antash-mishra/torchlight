# rank

> **Status:** M4 RRF baseline implemented; service integration and comparison pending
> **Source:** `src/index/rank.c` · **Header:** `include/torchlight/rank.h`
> **Tests:** `tests/unit/test_rank.c`, `tests/alloc/query.c`

## Purpose

Fuses already-ranked lexical and semantic lists while explicitly protecting
exact paths/names. This is the first M4 comparison baseline; model evaluation
and held-out comparison with score combination have not selected a default.
M5 personalization remains future work.

## Public API and ownership

`rank_create` allocates scratch for at most 2000 combined input occurrences and
a positive RRF constant. Start with `RANK_DEFAULT_RRF_K` (60).
Each concurrent query needs its own ranker; `rank_destroy` releases scratch.

`rank_fuse` accepts best-first candidate lists containing nonzero ids, borrowed
raw path strings and explicit regular/exact-basename/exact-path priorities.
It deduplicates matching ids across the two lists and adds
`1 / (k + one-based source rank)` for each source. Duplicate ids within one list
are rejected so a candidate cannot vote twice. Equal ids with differing paths
return `TL_STATE` rather than combine incompatible catalog views.

Exact paths precede exact basenames, which precede regular fusion scores, even
when ordinary RRF scores would win. Equal scores preserve lexical source order,
then compare raw paths and ids. With an empty semantic list, output preserves
the lexical order exactly. Unembedded lexical entries remain candidates.

Output borrows the input path lifetimes. No allocations, SQL or I/O occur during
fusion; errors zero the output count and leave scratch reusable. Input and
output arrays must not overlap. Use a fixed candidate pool independent of the
display limit, or larger input lists can legitimately change RRF ranks/scores.

## Design and boundaries

Sorting and grouping in preallocated scratch give `O(C log C)` work and `O(C)`
memory for `C` input occurrences. Generic allocation-free heapsort lives in
`core/sort`, shared with vector result ordering.

The caller supplies exact-match evidence from retrieval; the ranker does not
infer names from lossy display strings. Future service orchestration must map
lexical results and semantic ids under the same pinned catalog/desktop view.
Matching paths alone do not verify desktop revisions or `emb_gen` coherence.

`make bench-vector` includes fusion of two 1000-candidate lists. Recorded p95
was 0.660/0.567 ms in the 50k/500k vector benchmark processes on 2026-10-05.
These runs establish fusion cost, not semantic relevance or final-response time.

## Testing

Tests check analytic RRF scores, candidates occurring in one/both lists,
unembedded entries, weaker exact hits preceding higher fused scores, lexical
fallback, stable ties, raw byte paths, 64-bit ids, result limits, duplicate ids,
conflicting identities, capacity overflow and reuse after errors. Allocator
interposition covers maximum-size input lists.

## Related

- [lexical](../lexical/README.md)
- [vector](../vector/README.md)
- [M4 implementation](../../m4-implementation.md)
