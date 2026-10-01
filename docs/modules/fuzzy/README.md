# fuzzy

> **Status:** Partially implemented: greedy subsequence scorer
> **Source:** `src/index/fuzzy.c` · **Header:** `include/torchlight/fuzzy.h`
> **Tests:** `tests/unit/test_fuzzy.c`

## Behavior and ownership

`fuzzy_score` is a pure allocation-free scorer: greedy earliest ordered matches
receive separator/camelCase and consecutive bonuses, with bounded gap penalties.
It checks masks and returns zero unless every query symbol matches. Numeric
bonuses are named constants. The engine handles basename priority separately.

This first scorer is a readable baseline, not optimal dynamic-programming fzy
alignment. It does not correct substitutions or inserted query characters.
Bounded edit scoring belongs in the subsequent typo increment. Tests compare
boundary matches to scattered matches, reject nonmatches, and validate errors.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
