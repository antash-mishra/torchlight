# prefix

> **Status:** Implemented (M1)
> **Source:** `src/index/prefix.c` · **Header:** `include/torchlight/prefix.h`
> **Tests:** `tests/unit/test_prefix.c`

## Behavior and ownership

The opaque prefix index sorts keys by symbol order and slot: the whole basename
(`PREFIX_BASENAME_SCORE`), each separator/word-boundary token
(`PREFIX_TOKEN_SCORE`, or `PREFIX_PARENT_SCORE` before `text.basename`) and the
basename initials (`projectNotes.md` → `pnm`, `PREFIX_INITIALS_SCORE`). Basename
and token keys borrow the caller's normalized symbols; initials live in one
index-owned arena and get pointers only at `prefix_finish`, after the arena
stops moving.

`prefix_query` finds the matching key range with two binary searches and reports
every key in it through a callback, without further comparisons and without
truncating by slot. A slot can be reported once per matching key; callers keep
the maximum. `lexical` builds one index over basenames and one over directory
names.

Tests cover lifecycle errors, callback stops, every key kind and its score, and
basename-over-parent priority. Build sorting is O(K log K); a query is
O(log K + matching keys).

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- Public headers document parameters, lifetimes and error contracts.
