# M4 implementation

M4 has started with the model-independent C foundation. The daemon and popup
still use lexical search. No trained model or inference runtime has been
installed, and M4's quality/latency acceptance is not complete. English is the
first model evaluation target, as selected by the user.

## Work completed

- [x] Swappable embedder adapter with owned full `emb_gen` metadata,
  query/document roles, validated normalized output and failure ownership.
- [x] Immutable owned float cosine reference with fixed row/byte bounds,
  preallocated query scratch and deterministic top-k ordering.
- [x] Bounded RRF baseline starting at k=60, explicit exact-path/name priority,
  cross-list deduplication, incompatible-path rejection and lexical fallback.
- [x] Independent scalar/analytic tests, invalid input/lifecycle/overflow
  regressions, concurrent vector readers and allocator interposition.
- [x] Synthetic 50k/500k float scan and 2×1000-candidate fusion benchmark.

Public contracts are in [embed.h](../include/torchlight/embed.h),
[vector.h](../include/torchlight/vector.h) and
[rank.h](../include/torchlight/rank.h). Module docs explain their boundaries;
[ADR 0022](adr/0022-m4-semantic-foundation.md) records the initial design.

## Remaining implementation sequence

1. **Model and representation evaluation.** Expand the labeled mixed fixture
   with English semantic paraphrases for files, folders, apps and settings,
   keeping calibration/tuning/held-out target families separate. Compare
   Potion retrieval 32M and Granite small English R2, retaining BGE small as
   a baseline. Embed names, extensions, nearby folder context and explicit
   application metadata; compare common-folder noise and abbreviation handling.
   Use actual model outputs with the float reference. Compare lexical,
   semantic-only, RRF and tuned score combination using candidate recall,
   first useful result, top-ten success and nDCG@10 per query class.
2. **Backend and production vector format.** Validate C tokenizer/inference
   parity with pinned reference outputs. Discuss a runtime/tokenizer dependency
   before adding one. Measure query inference, index throughput, allocations
   and total/peak RSS. Evaluate dimensional reduction and binary Hamming plus
   scaled int8 rescoring against both the original float reference and labeled
   relevance. Tune the shortlist; add ANN only if measurements require it.
3. **Persistence and background indexing.** Add migrations in `store.c` for
   versioned/staged vectors and active `emb_gen` configuration. Embed changed
   paths/application metadata off the query thread; reject work for stale ids,
   path versions or desktop revisions. Bound queues and memory, prioritize
   interactive queries, recover after restart, and validate a replacement
   completely before activation.
4. **Coherent two-phase service.** Pin file, desktop metadata and embedding
   snapshots across both phases. The current desktop lease is an exclusive
   mutex lease and cannot be held through inference; it needs a lifetime-safe
   immutable snapshot lease. Send lexical results immediately, enqueue bounded
   semantic work, then fuse for the same request/search ids and snapshot pair.
   Cancel queued obsolete work per client and discard running obsolete output.
   Missing models, failure, saturation and deadline expiry must all terminate
   with lexical fallback/status. Slow clients must not retain snapshots forever.
5. **Client and milestone acceptance.** Verify terminal CLI responses,
   stale suppression and deliberate popup selection across reordering. Test
   updates/renames/deletes, desktop replacement, incomplete embeddings, model
   replacement, cancellation, slow clients, failures and restarts. Measure
   both phases from request acceptance, including queue wait/inference, engine
   and IPC latency separately, first query, indexing load, update lag and
   steady/peak RSS at 50k/500k. Retain exact-name/path and raw-byte regressions.

## Initial measurements

2026-10-05, Intel i7-8700K, approximately 32 GB RAM, Linux Mint 22.2 /
kernel 6.8.0-139-generic, GCC 13.3.0, C17 `-O3 -DNDEBUG` with the repository
warning flags. Shared host, unpinned process. Deterministic synthetic floats,
256 dimensions, 64 timed warm queries after one first scan, top ten. No trained
model/tokenizer, filenames, inference, lexical search, IPC or background load
was included. Warm buffers are not an OS residency guarantee.

| Rows | Reserved index bytes | Build (s) | First scan (ms) | Scan p50 / p95 / p99 (ms) | Resident / peak RSS (KiB) |
|---:|---:|---:|---:|---:|---:|
| 50,000 | 51,600,064 | 0.075 | 12.992 | 15.476 / 21.171 / 24.768 | 52,304 / 52,224 |
| 500,000 | 516,000,064 | 0.735 | 159.029 | 149.671 / 161.457 / 217.347 | 505,840 / 505,856 |

Resident RSS comes from `/proc/self/statm`; peak comes from `getrusage`. These
interfaces can differ slightly in page accounting/sampling. Both sizes passed
64/64 self-match checks, with zero minor/major faults during the warm samples.
That tests generated vectors, not semantic relevance or quantized recall.

Fusion of two 1000-entry lists had p50/p95/p99 of 0.368/0.660/0.750 ms in the
50k process and 0.314/0.567/0.731 ms in the 500k process. Its scratch cost is
bounded by candidate count, independent of vector row count.

The 500k float reference exceeds both the final-phase 10 ms target and the
planned 150 MB compressed-vector budget before inference. Production-format
evaluation is therefore necessary; this does not select a compressed format or
establish its recall. The float reference need not remain loaded in production.

Run `make bench-vector`; raw output is in
[the benchmark artifact](../tests/bench/results/2026-10-05-m4-foundation-vector.txt).

## Verification

`make`, `make test`, `make lint`, `make bench` and `make bench-vector` pass.
ASan/UBSan/leak checks, allocator interposition and CLI/desktop/daemon integration
cover the new foundation and the existing launcher. Clang-tidy and cppcheck
report no project warnings. Changed C files pass the formatting check, and
`AGENTS.md` remains identical to `CLAUDE.md`.

The existing lexical benchmark passes all nine fixtures at each size. Its 28
tuning/held-out quality rows exactly match the recorded first-token-completeness
baseline. No lexical query algorithm or active daemon was changed.

| Paths | Typing p50 / p95 / p99 (ms) | Leased query p50 / p95 / p99 (ms) | Steady / peak RSS (KiB) |
|---:|---:|---:|---:|
| 50,000 | 0.053 / 0.956 / 2.872 | 0.107 / 1.075 / 3.331 | 39,964 / 46,396 |
| 500,000 | 1.183 / 7.799 / 13.651 | 1.356 / 7.266 / 13.491 | 334,748 / 399,192 |

These are unpinned warm synthetic engine observations on the shared reference
host. Held-out aggregate Recall@1 / Recall@10 / MRR@10 / candidate Recall@1000
is 0.462 / 0.607 / 0.508 / 0.978 at 50k and
0.373 / 0.539 / 0.424 / 0.852 at 500k. They do not measure hybrid search or broad
semantic relevance; the original lexical 5 ms target remains deferred.
Raw output: [lexical benchmark](../tests/bench/results/2026-10-05-m4-foundation-lexical.txt).
