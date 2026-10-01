# embed

> **Status:** Planned
> **Source:** `src/index/embed.c` · **Header:** `include/torchlight/embed.h`
> **Tests:** `tests/unit/test_embed.c`

## Purpose
Produces embeddings for paths and queries behind the swappable `tl_embedder`
interface. Backend and default model are selected by M4 evaluation.

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
- Candidates: `potion-retrieval-32M`, `granite-embedding-30m-english`,
  `bge-small-en-v1.5`. Benchmark labeled filename/path queries before choosing.
- Evaluate native static lookup/pooling and supported ONNX export; validate
  tokenization and numerical parity with reference embeddings.
- Model revision, tokenizer/preprocessing, dimension, and quantization format
  define an embedding generation (`emb_gen`). Stage and validate replacements before activation.
- Output dimension must fit the vector memory budget (see `vector`); dimension
  reduction is evaluated against full-dimensional quality. Any fitted projection
  is calibrated separately from held-out evaluation, versioned/checksummed in
  `emb_gen`, and applied identically to queries and paths.
- Path changes invalidate embeddings. Discard work for stale path/model versions.
- Semantic matches rely on useful names/parent context. Abbreviation recognition
  and general published retrieval scores do not guarantee filename quality.

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

## How to add a new model
1. Export it with `scripts/` into `models/`.
2. Validate the tokenizer/inference pipeline against the model's reference.
3. Implement or configure a `tl_embedder` backend.
4. Stage a new `emb_gen`; persist its configuration and activate after validation.
5. Record filename quality, full query latency, allocations, and total/peak RSS.

## Related
- [vector](../vector/README.md)
- [tokenize](../tokenize/README.md)
- [store](../store/README.md)
