# subseq

> **Status:** Implemented (M1): mask filter, ordered match, membership cache
> **Source:** `src/index/subseq.c` · **Header:** `include/torchlight/subseq.h`
> **Tests:** `tests/unit/test_subseq.c`

## Behavior and ownership

`subseq_matches` is a stateless pure predicate over normalized borrowed views.
A conservative symbol mask rejects impossible matches before an ordered scan.
All symbols, including opaque malformed bytes, contribute mask bits; collisions
only add scan work. (`lexical` scores subsequences with `fuzzy_score`, which
performs the same ordered check.)

`tl_subseq_cache` holds the complete subsequence membership of one previous
word for incremental narrowing. `subseq_cache_lookup(word, mode)` returns it
when the cached word is a symbol prefix of `word` and was recorded with the same
caller-defined mode: a subsequence of an extended word is a subsequence of its
prefix, so only those slots can match. `subseq_cache_begin` hands out a second
buffer, so a scan reads the old membership while recording the new one;
`subseq_cache_commit` swaps them. Callers must record every matching slot; the
cache never narrows from ranked or truncated results.

`lexical` uses mode = "word contains `/`", because such words match across the
full path and membership is not monotone across that switch. The cache lives in
a workspace bound to one immutable engine, so `catalog_gen` and matching rules
cannot change underneath it.

Tests cover `prjnts`, reversed order, malformed-byte identity, mask collisions,
prefix/mode lookup rules, non-aliasing buffers and capacity limits.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- Public headers document parameters, lifetimes and error contracts.
