# typo

> **Status:** Implemented (M1)
> **Source:** `src/index/typo.c` · **Header:** `include/torchlight/typo.h`
> **Tests:** `tests/unit/test_typo.c`

## Purpose
Typo-tolerant candidate channel: finds names containing a complete token exactly
one edit away from the query word (insertion, deletion, substitution or adjacent
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
exclusion, camelCase tokens, digit exclusion, two-edit and too-short queries.

## Gotchas
- Typos in *incomplete* words (`projc` for `project…`) are not one edit from a
  complete token; the trigram channel covers some of them. Prefix typo correction
  remains a later extension (see [evaluation](../../evaluation.md)).
- Only basename tokens are indexed, not directory names.

## Related
- [lexical](../lexical/README.md), [fuzzy](../fuzzy/README.md), [trigram](../trigram/README.md)
- ADR: [0008](../../adr/0008-m1-completion-channels-directories-config.md)
