# trigram

> **Status:** Planned
> **Source:** `src/index/trigram.c` · **Header:** `include/torchlight/trigram.h`
> **Tests:** `tests/unit/test_trigram.c`

## Purpose
Trigram inverted index used by `lexical` for substring and typo-tolerant candidate retrieval. Only the trigram channel: prefix, subsequence and typo channels live in their own modules.

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
- Byte-level trigrams over normalized names. Keys are collision-checked hashes
  (or byte tuples); arbitrary Unicode codepoint triples do not pack into 32 bits.
- Relaxed overlap: a path qualifies if it shares at least a configurable
  fraction of query trigrams, not all of them.
- Posting lists hold stable file ids. Benchmark compression and update cost
  before choosing a representation.
- Built privately and published as part of a catalog generation (`catalog_gen`).

## Data flow
_TODO after implementation._

## Invariants
_TODO after implementation._

## Performance
_TODO: complexity, memory use, `make bench` numbers with date._

## Testing
_TODO after implementation._

## Gotchas
_TODO after implementation._

## Related
- [lexical](../lexical/README.md)
- [tokenize](../tokenize/README.md)
- [store](../store/README.md)
