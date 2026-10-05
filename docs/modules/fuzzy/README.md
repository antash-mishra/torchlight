# fuzzy

> **Status:** Implemented (M3 Part 2): bounded optimal alignment and one-edit distance
> **Source:** `src/index/fuzzy.c` · **Header:** `include/torchlight/fuzzy.h`
> **Tests:** `tests/unit/test_fuzzy.c`

## Behavior and ownership

`fuzzy_score` is pure and allocation-free. For text of at most 512 symbols it
finds the best alignment under the original boundary/consecutive bonuses and
capped gap penalties. Two fixed stack rows and running maxima make the DP
O(text × query); an ordered-membership pass rejects impossible matches early.
Larger text uses `fuzzy_score_greedy`, also available as a comparison baseline.
`fuzzy_score_bound(n)` remains unchanged and bounds both scorers.

`fuzzy_edit_distance` is the separate bounded edit scorer used by the typo
channel. It returns the optimal-string-alignment distance when it is at most
`FUZZY_EDIT_LIMIT` (1), else 2. Insertion, deletion, substitution and an adjacent
swap each cost one, so `raedme` is one edit from `readme`. With a one-edit budget
everything before the first mismatch must be equal, so the check is linear and
needs no table.

Tests compare 2048 small cases against an exhaustive alignment oracle, require
optimal scores to dominate greedy scores, and cover later better alignment,
capped gaps, the long-text fallback, all edit kinds and score/error bounds.
See [ADR 0020](../../adr/0020-m3-part2-search-quality.md) and the
[completion measurements](../../m3-part2-completion.md).

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- Public headers document parameters, lifetimes and error contracts.
