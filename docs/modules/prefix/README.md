# prefix

> **Status:** Implemented (M3 Part 2): token-completeness scoring, including the first basename token; M5 per-text scoring
> **Source:** `src/index/prefix.c` · **Header:** `include/torchlight/prefix.h`
> **Tests:** `tests/unit/test_prefix.c`

## Behavior and ownership

The opaque prefix index sorts keys by symbol order and slot: the whole basename
(`PREFIX_BASENAME_SCORE`), each separator/word-boundary token
(`PREFIX_BASENAME_SCORE` at `text.basename`, `PREFIX_TOKEN_SCORE` for later
basename tokens, or `PREFIX_PARENT_SCORE` before `text.basename`). Complete
basename token matches gain `PREFIX_COMPLETE_BONUS` (128) at their own tier;
shorter fragments receive no bonus. This keeps a whole-basename prefix from
overriding a completed first token. It also indexes
basename initials (`projectNotes.md` → `pnm`, `PREFIX_INITIALS_SCORE`). Basename
and token keys borrow the caller's normalized symbols; initials live in one
index-owned arena and get pointers only at `prefix_finish`, after the arena
stops moving.

`prefix_query` finds the matching key range with two binary searches and reports
every key in it through a callback, without further symbol comparisons and without
truncating by slot. A slot can be reported once per matching key; callers keep
the maximum. `lexical` builds one index over basenames and one over directory
names, plus a separate index for explicit auxiliary fields.

Tests cover lifecycle errors, callback stops, every key kind and its score,
first-token completion and fragments extending past that token, and
basename-over-parent priority. Build sorting is O(K log K); a query is
O(log K + matching keys).

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [M1 completion ADR](../../adr/0008-m1-completion-channels-directories-config.md)
- [First-token completion fix ADR](../../adr/0021-first-token-completeness.md)
- Public headers document parameters, lifetimes and error contracts.

## Per-text scoring (M5)

`prefix_score(text, query, &best, &complete)` scores one text's keys without
an index. It reports what `prefix_query` would report for that text's slot:
the best key score, and whether any reported key is a complete token. It
enumerates keys with the same visitor `prefix_add` uses (basename, every
token, then initials), so both always see the same keys. The lexical side
pass uses it to build one-symbol evidence for a few boosted entries without
expanding every key that starts with the symbol. `test_prefix.c` checks it
against `prefix_query` for every slot and query.
