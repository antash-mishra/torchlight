# fuzzy

> **Status:** Implemented (M1): greedy subsequence scorer and one-edit distance
> **Source:** `src/index/fuzzy.c` · **Header:** `include/torchlight/fuzzy.h`
> **Tests:** `tests/unit/test_fuzzy.c`

## Behavior and ownership

`fuzzy_score` is a pure allocation-free scorer: greedy earliest ordered matches
receive separator/camelCase and consecutive bonuses, with bounded gap penalties.
It checks masks and returns zero unless every query symbol matches. Numeric
bonuses are named constants. `fuzzy_score_bound(n)` is the largest score any
`n`-symbol query can get; `lexical` uses it to prove that skipped work cannot
change results.

`fuzzy_edit_distance` is the separate bounded edit scorer used by the typo
channel. It returns the optimal-string-alignment distance when it is at most
`FUZZY_EDIT_LIMIT` (1), else 2. Insertion, deletion, substitution and an adjacent
swap each cost one, so `raedme` is one edit from `readme`. With a one-edit budget
everything before the first mismatch must be equal, so the check is linear and
needs no table.

The scorer is a readable baseline, not optimal dynamic-programming fzy
alignment. Tests compare boundary and scattered matches, check bounds, reject
non-matches, cover all edit kinds and two-edit pairs, and validate errors.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- Public headers document parameters, lifetimes and error contracts.
