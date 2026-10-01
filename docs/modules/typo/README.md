# typo

> **Status:** Planned
> **Source:** `src/index/typo.c` · **Header:** `include/torchlight/typo.h`
> **Tests:** `tests/unit/test_typo.c`

## Purpose
Typo-tolerant candidate channel (one insertion/deletion/substitution) for queries the other channels miss.

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
- Default: a **deletion-neighbourhood index** (SymSpell style). Store each
  normalized basename, stem, and token with every one-character deletion; look
  up the complete query token and its one-character deletions. Verify candidates
  with edit distance; sharing a deletion key does not prove a one-edit match.
  Cost includes expanding dictionary/file postings; measure memory/update costs
  and recall as well as lookup time.
- Benchmark alternative: bit-parallel (Myers) edit-distance scan over
  resident basenames.
- Runs for query tokens of at least three characters, starting at one edit.
  An exact full-basename hit may skip it; counts of unrelated lexical hits may
  not. Measure candidate recall before adopting any additional performance gate.
- Distance operates on normalized codepoints/opaque-byte symbols, not UTF-8 byte
  counts. Typo correction of arbitrary incomplete prefixes is a later extension.
- Candidates are scored by the edit-distance scorer in `fuzzy`.

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
- [fuzzy](../fuzzy/README.md)
- [tokenize](../tokenize/README.md)
