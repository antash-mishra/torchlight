# 0031. M4 acceptance: prefix-shortlist vector search

- **Status:** Accepted; implemented
- **Date:** 2026-10-07

## Context

The remaining M4 gate is final-phase p95 below 10 ms at 500k paths. At the end
of M6 the final phase took 95 ms p95 at 500k, and the exhaustive int8 scan
alone took about 68 ms p50 (`2026-10-07-m6-vector.txt`). The scan reads every
row: 500k × 256 int8 components is 128 MB per query.

Three findings shaped the decision:

1. **The scan was compute-bound but is bandwidth-bound once vectorized.** An
   AVX2 int8 prototype took about 10 ms at 500k. On the reference i7-8700K, one
   thread reads about 12.7 GB/s and four threads at most about 22 GB/s, so no
   exhaustive 256d int8 scan gets far below 6 to 10 ms on this machine.
2. **The ADR 0002 sign-bit shortlist misses on recall.** It reached 0.9445 mean
   recall@10 with a 10k shortlist against exhaustive int8 (M4 evaluation).
3. **Potion's leading components are an embedding of their own.** Potion
   retrieval is Matryoshka-trained and our 256d export truncates it, so the
   first 128 components of each row are the model's own 128d embedding.

A sweep on trained Potion vectors of the 500k synthetic corpus (412 queries:
52 semantic fixture texts plus held-out launcher queries) measured recall@10
against exhaustive int8 for prefix cosine shortlists:

| Prefix | 2000 | 4000 | 8000 | 16000 |
|---:|---:|---:|---:|---:|
| 96 | 0.9369 | 0.9648 | 0.9825 | 0.9900 |
| 128 | 0.9816 | 0.9905 | **0.9956** | 0.9990 |
| 160 | 0.9954 | 0.9978 | 0.9990 | 1.0000 |

Normalizing each row's prefix (prefix cosine) beat the raw prefix dot product
at every size; 64 or fewer components lost too much at 500k.

## Decision

Add `vector_create_prefix_int8`, a two-pass int8 format, and use it in the
semantic service for models that declare nested prefixes.

- **Storage.** Rows keep the `int8-l2-1` quantization, split into a contiguous
  head (first `prefix` components) and tail (the rest), plus one float prefix
  inverse norm per row (2 MB at 500k). `emb_gen` and the persisted cache are
  unchanged; this is a search format.
- **First pass.** The query prefix is rounded to int16 at the largest scale
  that fits (exact integer dot products cannot overflow int32, by
  Cauchy-Schwarz, up to 4096 dimensions). Rows are scored in blocks of 64 by an
  AVX2 `madd` kernel when the CPU has it, else a portable loop with identical
  integers, so the shortlist is the same on every machine. The best rows are
  kept in a buffer of twice the shortlist that quickselect trims back to the
  shortlist, whose weakest entry then becomes the rejection threshold. The
  order is score, then position, so the kept set is unique.
- **Second pass.** Kept rows are rescored with the full int8 cosine. Cosines
  are bit-identical to `vector_create_int8`: a split row continues the head's
  four float accumulators over the tail, and the prefix is a multiple of 16.
- **Fallbacks.** A segment with at most `shortlist` rows, or a query whose
  prefix is all zero, scans exhaustively. Excluded rows never take a slot.
- **Service.** `tl_emb_model.nested_prefixes` declares the property; Potion
  sets it. The semantic service uses prefix 128 and shortlist 8000
  (`SEMANTIC_PREFIX_DIMENSIONS`, `SEMANTIC_SHORTLIST`) when the model has more
  than 128 dimensions, and exhaustive int8 otherwise. Derived snapshots copy
  base rows between indexes of the same prefix length.
- **Bit-identical SIMD rescoring.** An SSE4.1 kernel computes the same four
  lanes with the same conversions, products and sums as the portable loop. It
  serves every int8 format, so exhaustive int8 is also about twice as fast.

## Measurements (2026-10-07, i7-8700K, load average about 4)

`make eval-vector` on the trained 256d model, ten results:

| Rows | Mean recall@10 | Worst | Top-1 agreement | Score mismatches | Exhaustive p50 / p95 | Two-pass p50 / p95 |
|---:|---:|---:|---:|---:|---:|---:|
| 50k | 0.9998 | 0.90 | 1.0000 | 0 | 3.35 / 5.83 ms | 2.18 / 5.45 ms |
| 500k | 0.9956 | 0.80 | 1.0000 | 0 | 35.5 / 56.7 ms | 9.38 / 23.0 ms |

At 500k the first pass takes about 9 ms (64 MB read plus candidate trims),
selection about 0.2 ms and rescoring 8000 scattered rows about 2 ms.

Resident hybrid daemon (`bench_daemon.py --model`, 1200 held-out queries,
release build), final-phase round trip from request acceptance:

| Catalog | End of M6 p50 / p95 / p99 | Now p50 / p95 / p99 | Lexical phase p95 |
|---:|---:|---:|---:|
| 49,569 | 7.45 / 13.5 / 16.8 ms | 3.43 / 9.95 / 13.9 ms | 2.84 ms |
| 494,362 | 68.8 / 95.0 / 104.8 ms | 12.8 / 24.3 / 33.7 ms | 8.05 ms |

RSS at 500k stays at about 603 MB; a 100-file update still publishes in about
117 ms and hybrid search serves it after 231 ms through a derived stage.
Artifacts: `tests/bench/results/2026-10-07-m4-prefix-eval.txt`,
`-m4-prefix-vector.txt`, `-m4-prefix-hybrid-daemon.jsonl`.

## Alternatives considered

- **Faster exhaustive scan only** (SIMD, threads). Exact, but bounded by memory
  bandwidth near the whole 10 ms budget on the reference machine.
- **Sign-bit shortlist** (ADR 0002). Rejected on recall.
- **Prefix 160, shortlist 2000.** Recall 0.9954 with a cheaper second pass, but
  the bandwidth-bound first pass reads 25% more bytes. Not timed; worth
  measuring if rescoring dominates on another machine.
- **Integer rescoring with a quantized query.** Faster, but it changes every
  cosine; keeping float rescoring keeps scores identical to the exhaustive
  baseline and its earlier parity checks.
- **Huge pages, sorted rescoring, deeper prefetch.** Measured; no net gain.
- **IVF/HNSW.** More state to maintain incrementally and tune, and an
  approximate first pass all the same; not needed for the measured recall.

## Consequences

- Semantic top-10 is approximate at large catalogs: 0.44% of exhaustive
  neighbors are lost on average at 500k, with no top-1 change in the sweep.
  Recall is measured against exhaustive int8, not human labels.
- A new model must declare `nested_prefixes` truthfully; a model without
  nested prefixes keeps the exhaustive scan. Other export sizes (512d) are
  untested and should be measured with `make eval-vector` before relying on
  them.
- The final phase is about five times faster at 500k but its p95 (24.3 ms)
  still misses the 10 ms gate on the loaded reference machine: the lexical
  phase (8 ms p95) and the vector search (about 9.5 ms p50, 16 ms p95) run one
  after the other. The next steps would be to start the semantic search while
  the lexical search runs instead of after it, and to run the first pass on
  more than one thread (measured bandwidth allows about 1.7x). Both are
  deferred: semantic search is parked as opt-in
  ([ADR 0032](0032-park-semantic-search.md)).
