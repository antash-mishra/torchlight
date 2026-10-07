# fuzzy

> **Status:** Implemented (M3 Part 2; M6 step 2 Frizbee matcher): bounded optimal alignment, SIMD matching on the same scale, and one-edit distance
> **Source:** `src/index/fuzzy.c` · **Header:** `include/torchlight/fuzzy.h`
> **Tests:** `tests/unit/test_fuzzy.c`, `tests/alloc/query.c`

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

## Frizbee matcher (M6 step 2)

`fuzzy_matcher_create(word)` compiles a word for Frizbee's SIMD Smith-Waterman
(vendored v0.13.0, see [third_party](../../../third_party/README.md));
`fuzzy_matcher_score(matcher, text, ascii, &score)` scores one text without
allocating. Frizbee's weights are derived from the portable constants (match
40, gap open 25, gap extend 1, word-start bonus 32, no case or exact bonus)
and scores map as `232 + frizbee`, so a run from the first symbol and any gap
up to 64 score exactly as `fuzzy_score`, and `fuzzy_score_bound` bounds both.
Membership equals `fuzzy_score`'s: ASCII case-insensitive matching is the
tokenizer's folding, and for other text the normalized symbols decide first.

Input: raw ASCII bytes when the caller has them (case kept, so camelCase earns
the capitalization bonus); otherwise the symbols encoded as UTF-8, with a
letter at a tokenizer boundary after a lowercase letter written in upper case.
The portable scorer handles words over `FUZZY_MATCHER_MAX_SYMBOLS` (64), texts
over `FUZZY_MATCHER_MAX_BYTES` (1024, where Frizbee's fallback allocates) and
opaque symbols. Compiling a matcher allocates (about 24 allocations and two
score matrices of `(word length + 1) * 4 KiB`); a matcher is per thread.

**Build decision.** `make` compiles the vendored crate offline with Cargo
through Torchlight-owned manifests (`third_party/frizbee-build`); no prebuilt
SDK or `cargo cbuild` is used. Rust 1.89+ is a build dependency.

**Measured** (500k corpus basenames, prefiltered by mask, AVX2): 55-190 ns per
call versus 150-1500 ns for the portable DP, 1.4-5x faster (larger for long
words). Tests check equality with `fuzzy_score` for runs and gaps, membership
over 3000 random ASCII/case/separator strings plus Unicode, raw versus encoded
input, every fallback and argument error. See
[ADR 0029](../../adr/0029-m6-frizbee-scoring-and-candidate-volume.md).
