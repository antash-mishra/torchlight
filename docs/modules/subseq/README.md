# subseq

> **Status:** Implemented scan baseline; narrowing planned
> **Source:** `src/index/subseq.c` · **Header:** `include/torchlight/subseq.h`
> **Tests:** `tests/unit/test_subseq.c`

## Behavior and ownership

`subseq_matches` is a stateless pure predicate over normalized borrowed views.
A conservative symbol mask rejects impossible matches before an ordered scan.
All symbols, including opaque malformed bytes, contribute mask bits; collisions
only add scan work. No opaque object is needed for this stateless operation.

The engine scans every entry for each word; it never narrows from top-k. Complete
membership caching and incremental narrowing are not implemented yet. Reusing a
workspace after query changes still performs a complete scan. Tests cover
`prjnts`, reversed character order, malformed-byte identity and mask collisions.
The warm benchmark measures this baseline at 50k and 500k entries.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
