# trigram

> **Status:** Implemented (M1; M6 shortest-list counting and reference decisions)
> **Source:** `src/index/trigram.c` · **Header:** `include/torchlight/trigram.h`
> **Tests:** `tests/unit/test_trigram.c`

## Purpose
Trigram inverted index used by `lexical` for typo-tolerant and incomplete-word
candidate retrieval: a name qualifies when it shares enough three-symbol
substrings with the query word (`projectntoes` → `projectNotes.md`). Only the
trigram channel: prefix, subsequence and typo channels live in their own modules.

## Responsibilities
- Owns posting lists from trigram keys to slots and per-query scratch.
- Does **not** score or rank: it reports `(slot, shared, total)` and `lexical`
  turns that into a score. It does not read paths, only normalized views.

## Public API
| Function | Description |
|---|---|
| `trigram_create()` / `trigram_destroy()` | Owned empty index. |
| `trigram_add(index, text, slot)` | Index the distinct trigrams of a view; slots strictly increasing. |
| `trigram_finish(index)` | Build posting lists and seal. |
| `trigram_scratch_create()` / `trigram_scratch_destroy()` | Per-querier counters. |
| `trigram_query(index, reference, scratch, query, hit, context)` | Report slots sharing at least half of the query's informative trigrams; a non-NULL `reference` index decides which trigrams are frequent. |

Text passed to `trigram_add` is not retained. Scratch belongs to one index and
must be destroyed before it; one scratch per concurrent querier.

## Design
- **Keys** are three normalized symbols packed into a `uint64_t` (21 bits each):
  every scalar value and opaque byte symbol is below 2^21, so keys are exact and
  never collide. They use the same symbols as subsequence and edit matching.
- **Build** interns keys through `tl_hashmap`, records `(trigram id, slot)`
  pairs (8 bytes each) and counting-sorts them into one posting array. Pairs
  arrive in slot order, so every posting list is sorted and duplicate-free.
- **Relaxed overlap:** a slot is reported when it contains at least
  `ceil(total / 2)` of the query's informative trigrams. Query trigrams absent
  from the corpus count as informative (a typo usually creates some).
- **Frequent trigrams** (in more than 1/8 of slots, floor 64) are ignored: they
  barely narrow and their posting lists would dominate query time (`txt`).
- **Minimum query:** three distinct trigrams (five symbols). Shorter words are
  covered by prefix, subsequence and the typo channel.
- Distinct keys are ordered with the core in-place heapsort. Query scratch is
  fixed before search; long words do not trigger libc sorting allocations.

## Data flow
`lexical_finish` adds every basename view; `lexical_query` queries each word and
records `1000 + 1000 * shared / total` as the word's trigram score for the slot.

## Invariants
- Posting lists are sorted by slot and contain each slot at most once per key.
- Scratch counters are all zero between queries, even after a callback error.

## Performance
Build is linear in total trigrams plus a counting sort. Query cost is the sum of
the informative posting lists plus the touched slots. Memory is 4 bytes per
posting plus 8 bytes per distinct key; see [evaluation](../../evaluation.md).

## Testing
`make test` runs `tests/unit/test_trigram.c`: overlap counts, too-short and
absent queries, slot-order validation, lifecycle errors and scratch reuse.
`tests/alloc/query.c` additionally interposes the glibc allocator during complete
lexical queries, including 130/131/256-byte words, Unicode and worker batches.
It runs separately from ASan so sanitizer interposition cannot hide libc calls.

## Gotchas
- Trigrams span separators (`e.m` in `readme.md`), matching the query's own
  separators.
- Only basenames are indexed; parent directory names use prefix/subsequence
  matching (see [lexical](../lexical/README.md)).

## Related
- [lexical](../lexical/README.md), [typo](../typo/README.md), [tokenize](../tokenize/README.md)
- ADR: [0008](../../adr/0008-m1-completion-channels-directories-config.md)

## M6 changes

**Shortest-list counting.** A slot sharing at least `needed` of the `present`
informative lists appears in one of the `present - needed + 1` shortest, so only
those are scanned for candidates; each longer list is counted per candidate by
binary search on its sorted postings, or by one pass over the list counting
only known candidates when they outnumber `length / log2(length)`. Reported
counts are exact (a brute-force test compares every slot and count). Slots are
reported in the order the shortest lists first reach them.

**Reference decisions.** A delta segment passes its base index as
`reference`: the base's posting counts decide frequency, so an entry gets the
same trigram score in either segment. See
[ADR 0030](../../adr/0030-m6-incremental-indexing.md).
