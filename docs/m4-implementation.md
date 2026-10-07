# M4 implementation

M4's functional English-first hybrid path is implemented and opt-in through
`torchlightd --model PATH.tlm`. Potion 256d runs natively in C; background caching,
versioned publication, RRF and two-phase IPC are connected. **M4 acceptance is
not complete:** the 500k latency targets and broader model/relevance comparison
remain open and are deferred while semantic search is parked
([ADR 0032](adr/0032-park-semantic-search.md)). See [model research and measured evaluation](m4-model-evaluation.md).

## Implemented

- Owned swappable embedder descriptors; pinned, checked native Potion export,
  WordPiece/normalization/pooling and trained-reference parity.
- English labeled file/application fixture, tuning/held-out families, dimensional
  reduction, lexical/semantic/RRF/weighted comparisons and native end-to-end test.
- Immutable float cosine correctness reference; exhaustive int8 production
  baseline; experimental binary/int8 shortlist evaluated against trained vectors
  at 500k and rejected for default use on recall/latency evidence.
- Exact path/name priority, fixed candidate pools, deterministic fusion and
  allocation-free lexical/vector/ranking scratch.
- Schema v4 staged model descriptors, persistent prepared-text cache, bounded
  background cache batches, reuse of unchanged texts and stale-row pruning.
- Background generation validation, model replacement, restart/cache recovery,
  owned file/desktop metadata snapshots and bounded reclamation. Cache BEGIN
  failures retry without discarding an unchanged partial embedding build.
- Immediate lexical phase, bounded per-client semantic jobs, coherent generation
  pairs, eventfd completion, cancellation and deadline/error/unavailable fallback.
- Reviewed output backpressure with retryable buffer limits, current status on
  both phases, and regular-file validation that keeps FIFO model paths from
  blocking shutdown; dedicated unit and daemon regressions cover these fixes.
- Analytic sanitizer/loader/vector/cache tests and resident lifecycle tests;
  synchronous CLI now waits for the final phase; async popup selection
  handling retains its existing regression coverage.
- Real Documents name-preservation check (37 eligible files, private names kept
  out of git), 50k/500k engine and resident RSS/latency measurements.

Contracts: [potion](../include/torchlight/potion.h),
[semantic](../include/torchlight/semantic.h), [vector](../include/torchlight/vector.h),
[rank](../include/torchlight/rank.h), [storage](../include/torchlight/store.h).
Decisions: [initial foundation](adr/0022-m4-semantic-foundation.md),
[native backend and publication](adr/0023-m4-native-potion-and-two-phase-search.md),
[response backpressure and model inputs](adr/0024-m4-response-backpressure-and-model-inputs.md),
[cache staging retries](adr/0025-m4-cache-staging-retries.md).

## Remaining acceptance work

1. Contextual Granite/BGE were researched but not locally evaluated. Compare
   them empirically before claiming best model selection; discuss any new C
   inference/runtime dependency. Expand human-labeled real semantic intents;
   the Documents check currently validates exact names, not semantic labels.
2. Reduce final 500k latency below 10 ms without losing reference/human relevance.
   Binary shortlists of 10k/20k are implemented but miss the measured recall gate.
   The service now uses a prefix shortlist (first 128 components, 8000 rows
   rescored exactly) with 0.9956 recall@10 against exhaustive int8 at 500k;
   final p95 fell from 95.0 to 24.3 ms at 500k and to 9.95 ms at 50k
   ([ADR 0031](adr/0031-m4-prefix-shortlist-vector-search.md)). Still open at
   500k, and deferred (ADR 0032): overlap the semantic search with the lexical
   phase and parallelize the bandwidth-bound first pass.
3. Optimize initial embedding/cache throughput and full staging/update cost;
   measure complete semantic update lag and model-replacement peak RSS at scale.
   Current file-publication lag and process RSS measurements are explicit in the
   report; they do not establish incremental semantic-update performance.
4. Validate deliberate GTK selection across trained-model reordering in a real
   desktop session, beyond existing pure-model/two-phase regression checks.
   More Unicode/tokenizer coverage is also needed before broader language claims.

These are acceptance/optimization gates, not unimplemented cache or daemon
scaffolding. M6 worker separation, selected Frizbee SIMD integration and
incremental indexing are the next implementation priority; these M4 gates remain
open and do not block starting M6. M5 personalization follows M6 and M4 acceptance.
See [ADR 0026](adr/0026-search-workers-simd-and-incremental-indexing.md).

## Verification

Run `make test`, `make lint`, `make bench`, `make bench-vector`, and with an
exported model `make eval-vector MODEL=build/models/potion-256.tlm`. Normal tests use
independent analytic models and no ML dependencies. Trained-model evaluation is
optional and reproducible from pinned artifacts; large model binaries stay in
ignored `build/models/`. Relevant artifacts are linked in the evaluation report.

The 2026-10-06 review fixes passed `make all`, `make test` (ASan/UBSan),
`make lint`, `make bench` and `make bench-vector`. Trained-model checks passed
all 108 encoding parity cases and all 52 int8 reference order comparisons;
native held-out nDCG@10 remained 0.969 and top-ten success remained 100% on the
small fixture. The popup also retained its degraded warning after applying
both actual daemon phases with an offline root. These fixes do not close the
large-catalog latency or broader model-comparison gates above.
