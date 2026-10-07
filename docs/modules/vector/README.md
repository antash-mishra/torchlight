# vector

> **Status:** M4 float/int8 retrieval and the two-pass prefix shortlist (service
> default for nested-prefix models) implemented; experimental binary shortlist remains gated
> **Source:** `src/index/vector.c` · **Header:** `include/torchlight/vector.h`
> **Tests:** `tests/unit/test_vector.c`, `tests/alloc/query.c`, `tests/bench/eval_vector.c`

## Purpose

Provides the exhaustive cosine correctness reference needed to evaluate local
embedding models and later compressed retrieval. It searches owned resident
floats without SQL, filesystem I/O or query-time heap allocation.

## Public API and ownership

`vector_create` reserves a fixed row capacity for one nonzero `emb_gen` and
1–4096 dimensions. A caller-supplied byte budget covers the index struct, ids
and float payload; allocator overhead, query scratch and other snapshots need
separate accounting. Arithmetic overflow and an exceeded budget return
`TL_LIMIT` before allocating. Zero-capacity indexes are valid.

`vector_add` copies and normalizes finite, nonzero embeddings. Ids must be
nonzero and strictly increasing. Dimensions and `emb_gen` must match; a wrong
`emb_gen` returns `TL_STATE`. Failed additions leave the builder usable.
`vector_finish` seals the index, including an empty index. `vector_position`
finds a row by id, and `vector_add_row` appends another index's stored row
(same `emb_gen`, dimensions and format) without re-normalizing or
re-quantizing, so the copy scores bit-identically; the semantic worker uses it
to reuse unchanged rows across stages (M6 step 3c).

`vector_workspace_create` allocates normalized-query scratch before searching.
The workspace borrows the index; separate workspaces permit concurrent searches.
Destroy them before `vector_destroy`. `vector_workspace_exclude` attaches a
borrowed bitmap of row positions that queries on that workspace skip; a derived
semantic snapshot uses it to hide the base rows it replaced or removed (M6
step 3c). Unit tests exclude and restore a row in every format.

`vector_query` normalizes the query, visits every stored row, retains the best
1–1000 requested hits in the caller's result buffer and orders them by cosine,
then increasing id. Ties and retained results do not depend on output capacity.
`vector_normalize` also supports exact in-place normalization, validates before
writing, and accumulates in double precision to handle finite float extremes.
Scores describe normalized float vectors; they include float-rounding error.

## Consistency and memory

An index contains only one `emb_gen`. The caller must assign it to a complete
model/tokenizer/preprocessing/transform/format descriptor. Binding vectors to
file/desktop revisions and pinning catalog/embedding snapshots across response
phases belong to the implemented semantic service. An immutable vector builder alone does not supply
those lifetime guarantees.

Owned buffers avoid explicit query-time file reads; the OS can still page or
swap them. The float reference is for evaluation and exceeds the planned 150 MB
compressed-vector budget at 500k paths. The service uses the prefix shortlist
for models with nested prefixes and exhaustive int8 otherwise; the binary
shortlist remains experimental.

## Performance and next experiments

Construction reserves `O(rows × dimensions)` storage. Searching takes
`O(rows × dimensions + rows × log(k) + k × log(k))` worst-case work and uses
`O(dimensions)` workspace plus the caller's `O(k)` output heap.

`make bench-vector` measures synthetic 256-dimensional float scans and RRF at
50k/500k rows. On 2026-10-05, scan p95 was 21.171/161.457 ms; reserved index
bytes were 51,600,064/516,000,064. These are correctness-reference measurements,
excluding trained-model inference, names, lexical search and IPC. See
[M4 measurements](../../m4-implementation.md#initial-measurements).

Evaluate sign-bit Hamming shortlisting plus scaled int8 rescoring against this
reference and held-out relevance. The starting shortlist of 200 is an
experiment. At 256 dimensions, binary plus int8 payload is 144 decimal MB for
500k rows, excluding ids/scales and other state. Reduction must be compared with
full-dimensional quality; version any projection and apply it identically to
queries/documents. ANN remains conditional on measured latency and recall.

## Testing

Independent long-double cosine and insertion ordering check every output
capacity on a deterministic fixture. Tests cover finite float extremes, invalid
vectors, unchanged error output, copied input, budgets/overflow, empty indexes,
wrong dimensions/`emb_gen`/workspace, lifecycle, ties and concurrent readers.
Allocator interposition covers full 1000-hit searches and fusion; sanitizers
cover ownership and bounds. Synthetic self-match checks are not model relevance.

## Related

- [embed](../embed/README.md)
- [rank](../rank/README.md)
- [ADR 0022](../../adr/0022-m4-semantic-foundation.md)

## M4 integration

See [native backend/service decision](../../adr/0023-m4-native-potion-and-two-phase-search.md)
and [measured model evaluation](../../m4-model-evaluation.md).

## Compressed variants

`vector_create_int8` rounds normalized components with scale 127, stores integer
rows and inverse integer norms, then performs exhaustive normalized cosine.
Each lane of four float accumulators sums the products of every fourth
component in order; an SSE4.1 kernel (runtime-checked) performs the same lane
operations, so scores are bit-identical with or without it. It roughly halves
the exhaustive scan (35.5 ms p50 at 500k on trained vectors, from about 68 ms).
`vector_create_binary_int8` additionally stores sign bits and a fixed shortlist.
Two histogram passes retain the closest Hamming candidates, ties by id, before
int8 cosine; its top-k is approximate. Shortlist selection does not depend on
display capacity. Runtime-checked x86 POPCNT has a portable fallback. Budgeting
includes ids, components, inverse norms and signs. Both paths use owned memory
and preallocated scratch. [Measured recall/latency](../../m4-model-evaluation.md)
rejects the current binary variants as service defaults.

## Prefix shortlist (two-pass int8)

`vector_create_prefix_int8` keeps the `int8-l2-1` rows but stores each row's
first `prefix_dimensions` components (a positive multiple of
`VECTOR_PREFIX_BLOCK`, below the dimension count) in a contiguous head array and
the rest in a tail array, plus one float prefix inverse norm per row. With more
searchable rows than `shortlist`:

1. The normalized query prefix is rounded to int16 at the largest scale that
   fits. Exact integer dot products with the int8 heads cannot overflow int32
   (Cauchy-Schwarz bound, up to 4096 dimensions). Rows are scored in blocks of
   64 by an AVX2 kernel when available, else a portable loop with the same
   integers, and multiplied by the prefix inverse norm: a prefix cosine.
2. Candidates go to a workspace buffer of twice the shortlist. When it fills,
   quickselect (median-of-three, heap-sort fallback after 64 rounds) keeps the
   best `shortlist`, ordered by score then position, and the weakest kept row
   becomes the threshold later rows must beat. The kept set is unique for a
   query, so results do not depend on CPU features or buffer layout.
3. Kept rows are rescored with the full int8 cosine, prefetched a few rows
   ahead, and the result heap orders them as usual. A split row continues the
   head's accumulators over its tail, so every cosine is bit-identical to
   `vector_create_int8`.

A segment with at most `shortlist` rows, or a query whose prefix is all zero,
is scanned exhaustively; excluded rows never take a slot. Query output
capacity may not exceed the shortlist (`TL_LIMIT`). Rows copy between prefix
indexes of the same prefix length (`vector_add_row`); the shortlist may differ.
Memory adds 4 bytes per row; workspaces add `2 × shortlist` candidates (16
bytes each) and an int16 query prefix.

The top-k is approximate and only meaningful when leading components form an
embedding (Matryoshka truncation or a variance-ordered projection). On trained
Potion 256d vectors of the 500k synthetic corpus, prefix 128 with an 8000-row
shortlist kept mean recall@10 0.9956 (worst query 0.80) and top-1 agreement
1.0 against exhaustive int8, with p50 9.4 ms instead of 35.5 ms; at 50k recall
was 0.9998. See [ADR 0031](../../adr/0031-m4-prefix-shortlist-vector-search.md).

`make bench-vector` times the three int8 variants on random vectors (latency
only: random vectors have no nested prefix). `make eval-vector MODEL=...`
measures recall, top-1 agreement, score identity and latency on a trained
model. Unit tests cover equality with exhaustive int8 at every capacity when
the shortlist covers all rows, analytic first-pass selection, ties by
position, exclusions, the zero-prefix fallback, budgets, row copies and the
largest dimension; the allocation test covers repeated two-pass queries.
