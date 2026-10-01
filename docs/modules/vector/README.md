# vector

> **Status:** Planned
> **Source:** `src/index/vector.c` · **Header:** `include/torchlight/vector.h`
> **Tests:** `tests/unit/test_vector.c`

## Purpose
Semantic nearest-neighbour search with a float cosine reference and an evaluated
binary/int8 optimization.

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
- Reference: normalized float embeddings and cosine search.
- Candidate optimization: sign-bit Hamming scan, then int8 rescoring. Persist
  scales/normalization metadata so rescoring is comparable across entries.
- Top 200 is a starting shortlist experiment. Tune against reference recall.
- Benchmark total latency at 50k/500k paths; brute-force timing is unproven.
  Consider ANN only if measured latency or recall warrants it.
- **Preload vectors into owned memory; no file-backed query mapping.** This avoids
  explicit reads, not OS page faults/swapping. Warm buffers and measure memory
  pressure. No hard paging-free guarantee is made.
- Memory budget: binary + rescoring payload ≤ 150MB at 500k paths (configurable).
  To meet it, reduce dimensions first (PCA for Model2Vec, truncation for
  Matryoshka-trained models), measured against both original full-dimensional and
  reduced float references. Fit transformations on calibration data, persist
  their checksum/configuration in `emb_gen`, and apply identically to queries and
  paths. At 256
  dimensions, binary + int8 is 16MB + 128MB = 144MB. If quality needs more
  dimensions, evaluate int4 rescoring, a larger configured budget, or binary-only
  retrieval's own quality. A larger shortlist without rescoring cannot restore
  information lost in binary quantization.
- Measure total/peak RSS, including metadata, lexical/typo indexes, per-client
  caches, and staged/old snapshots. Defer replacement if staging exceeds budget.
- Never mix embedding generations (`emb_gen`); publish replacements together after validation.

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
- [embed](../embed/README.md)
- [rank](../rank/README.md)
