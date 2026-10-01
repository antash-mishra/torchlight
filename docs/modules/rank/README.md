# rank

> **Status:** Planned
> **Source:** `src/index/rank.c` · **Header:** `include/torchlight/rank.h`
> **Tests:** `tests/unit/test_rank.c`

## Purpose
Fuses lexical and semantic result lists and applies personalization.

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
- Reciprocal Rank Fusion: `score = Σ 1 / (k + rank_i)`. Start with
  k = 60 (the common default) and tune it on labeled queries.
- Exact basename matches have explicit priority.
- Bounded frecency and query→file boosts from resident summaries of `opens`.
- Lexical-only behavior when semantics is unavailable or entries lack embeddings.
- Two-phase: rank lexical results alone first; re-fuse when semantic results
  arrive for the same request id.
- No SQL during ranking. Compare held-out ranking quality with/without each layer.

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
- [vector](../vector/README.md)
- [store](../store/README.md)
