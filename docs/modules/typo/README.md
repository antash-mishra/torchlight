# typo

> **Status:** Implemented (M3 Part 2): whole-token and indexed prefix edits
> **Source:** `src/index/typo.c` · **Header:** `include/torchlight/typo.h`
> **Tests:** `tests/unit/test_typo.c`

## Purpose
Typo-tolerant candidate channel: finds names containing a complete token or an unfinished prefix one
edit away from the query word (insertion, deletion, substitution or adjacent
swap), e.g. `raedme` → `README.md`.

## Responsibilities
- Owns a dictionary of distinct name tokens, their slot postings and a
  deletion-key table, plus per-query scratch.
- Does **not** score: `lexical` gives every hit the same typo score. Exact token
  matches are left to the prefix channel.

## Public API
| Function | Description |
|---|---|
| `typo_create()` / `typo_destroy()` | Owned empty index. |
| `typo_add(index, text, slot)` | Record the eligible tokens of a view (symbols borrowed). |
| `typo_finish(index)` | Deduplicate tokens, build deletion keys, seal. |
| `typo_scratch_create()` / `typo_scratch_destroy()` | Per-querier term stamps. |
| `typo_query(index, scratch, query, hit, context)` | Report slots holding a token one edit from the query. |

Token symbols are borrowed from the caller's views and must outlive the index.

## Design
- **Dictionary:** tokens are split at separators and word boundaries (as the
  prefix channel does). Tokens of 3–32 symbols that are not all ASCII digits are
  indexed; numbers are excluded because digit typos rarely help and numeric
  tokens are mostly unique, which would bloat the table.
- **Deletion neighbourhood (SymSpell style):** each distinct token is stored
  under the hash of itself and of every one-symbol deletion. A query probes the
  hash of itself and of its one-symbol deletions. Any two strings within one
  insertion, deletion, substitution or adjacent swap share a key.
- **Verification:** shared keys are only candidates. Each candidate term is
  checked once per query (epoch stamps) with `fuzzy_edit_distance`, which also
  removes hash collisions. Only distance exactly one is reported.
- Distances use normalized code points and opaque byte symbols, never UTF-8
  byte counts.
- Query words are also limited to **3–32 normalized symbols**, inclusively.
  Two-symbol and 33-symbol neighbours receive no typo retrieval or score;
  eligible three/31-symbol queries can still match three/32-symbol terms.

## Data flow
`lexical_finish` adds basename views; each query word without `/` probes the
channel and `lexical` records `LEXICAL_TYPO_SCORE` (3500) for every hit: below
every prefix hit, above subsequence-only matches.

## Invariants
- Terms are distinct; postings per term are sorted and duplicate-free.
- Deletion keys are sorted by hash; `(hash, term)` pairs are unique.

## Performance
Memory is about `(length + 1)` 16-byte keys per distinct token plus postings.
A query performs `length + 1` binary searches, verifies each candidate term once
and expands postings of matching terms. Expansion is proportional to how many
names contain the corrected token.

## Testing
`make test` runs `tests/unit/test_typo.c`: all four edit kinds, exact-token
exclusion, camelCase tokens, digit exclusion, two-edit queries and both length
boundaries. The lexical large-corpus regression checks a two-character
subsequence's analytic score so a typo boost cannot silently return.

## Indexed prefix edits

Distinct terms are sorted lexicographically, so each shared prefix is a
contiguous range (an implicit trie). Clipped OSA rows prune impossible branches;
binary searches find child ranges. At an accepted prefix, every term's postings
are expanded, with per-query stamps preventing duplicate term expansion.
Transpositions, substitutions, insertions and deletions are supported. There is
no candidate truncation and no additional persistent node table. Stack rows
bound traversal to query length + 1, at most 33 levels.

Prefix edits start at five symbols; the complete-token channel retains its
three-symbol minimum. Both stop at 32. Numeric-only queries are excluded as well
as numeric-only indexed tokens. Exact prefixes receive no prefix-edit boost,
though they can still be complete-token one-edit neighbours. Tokens longer than
32 symbols and directory-name typos remain outside this channel's scope.

Tests compare 729 query variants against an independent full OSA matrix, and
cover `proej`, all prefix edit kinds, exact-prefix exclusion and length bounds.
See [ADR 0020](../../adr/0020-m3-part2-search-quality.md).

## Related
- [lexical](../lexical/README.md), [fuzzy](../fuzzy/README.md), [trigram](../trigram/README.md)
- ADR: [0008](../../adr/0008-m1-completion-channels-directories-config.md)
