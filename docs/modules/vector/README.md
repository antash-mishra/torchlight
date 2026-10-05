# vector

> **Status:** M4 float reference implemented; quantization and service integration pending
> **Source:** `src/index/vector.c` · **Header:** `include/torchlight/vector.h`
> **Tests:** `tests/unit/test_vector.c`, `tests/alloc/query.c`

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
`vector_finish` seals the index, including an empty index.

`vector_workspace_create` allocates normalized-query scratch before searching.
The workspace borrows the index; separate workspaces permit concurrent searches.
Destroy them before `vector_destroy`.

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
phases are still service work. An immutable vector builder alone does not supply
those lifetime guarantees.

Owned buffers avoid explicit query-time file reads; the OS can still page or
swap them. The float reference is for evaluation and exceeds the planned 150 MB
compressed-vector budget at 500k paths. Production storage/search format is not
selected by this implementation.

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
