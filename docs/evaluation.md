# Evaluation

How Torchlight's search quality and performance are measured. Every milestone
validates against this list; performance numbers in `PLAN.md` are targets until
recorded here.

- Curate labeled queries for names, extensions, prefixes, abbreviations, short
  queries, insertions/deletions/substitutions, Unicode, non-UTF-8 bytes,
  empty/multiword queries, duplicate basenames, directory context, and semantic
  paraphrases. Include the `PLAN.md` examples (`prjnts` → `projectNotes.md`,
  `tax receipts` → `ITR_2024_ack.pdf`).
- Verify warm narrowed queries equal cold queries after appending, backspacing,
  normalization changes, and catalog updates. Include matches absent from the
  previous top results and typo targets obscured by unrelated lexical matches.
- Verify final responses on disabled/failed/timed-out semantics, slow clients,
  selection stability during reordering, stale opens, raw paths containing
  newlines, ids beyond JSON's exact numeric range, and duplicate requests.
- Use labeled relevance to compare original and reduced/quantized semantic
  quality; agreement with a reduced float reference alone is not enough. Keep
  projection fitting, ranking tuning, and held-out evaluation data separate.
- Track candidate recall, Recall@10, and reciprocal rank on held-out queries.
  Compare lexical, semantic, fused, and personalized ranking separately.
- Record CPU, RAM, OS, corpus size/path lengths, model revision, build flags,
  p50/p95/p99 latency, startup/indexing times, update lag, and steady/peak RSS.
- Include incomplete misspelled tokens (e.g. `projc` for `project…`). Typo
  lookup only matches complete tokens today, so this measures how much the
  trigram channel covers before prefix typo correction is added.

## Results

### 2026-10-02: first M1 prefix/subsequence baseline

Reference environment: Intel Core i7-8700K (6 cores / 12 threads), 32,033 MiB RAM,
Linux Mint 22.2, kernel 6.8.0-139-generic, x86-64. GCC 13.3.0,
`-std=c17 -O3 -DNDEBUG` plus all repository warning flags; SQLite 3.45.1 and
utf8proc 2.9.0. No semantic model or embedding measurement applies.

`make bench` builds resident synthetic paths (mean 44 bytes), then allocates
scratch and times one first query plus 30 rounds of eight cases. Cases include
an exact basename, prefix, abbreviation, initials, one-character query,
multiword parent context, canonical Unicode equivalent and a no-match query.
Seven expected targets survive top-10 in every round; 30 no-match checks also
pass (240/240 total checks at each size). This is a synthetic correctness
baseline, **not held-out relevance or candidate-recall validation**. Typos are
not supported or measured in this increment.

| Paths | Build + scratch (s) | First query (ms) | Warm p50 (ms) | Warm p95 (ms) | Warm p99 (ms) | Peak RSS (KiB) |
|---:|---:|---:|---:|---:|---:|---:|
| 50,000 | 0.322 | 3.061 | 4.085 | 12.594 | 15.875 | 72,832 |
| 500,000 | 4.093 | 29.937 | 45.525 | 101.793 | 146.039 | 708,992 |

Warm timing excludes process startup, SQLite loading, corpus construction and
scratch allocation. Peak RSS includes construction/sorting and resident engine;
steady RSS has not been isolated. Runs were unpinned on a shared development
machine under memory/swap pressure; figures are observations, not guarantees.
Raw output is in [benchmark results](../tests/bench/results/2026-10-02-m1.txt).

The 5 ms p95 target is **not met**. The initial full scan and key/metadata layout
need traversal/memory improvements before M1 acceptance. Runtime allocations
are kept outside lexical queries by implementation, but an allocator-interception
check remains useful future evidence. CLI startup, filesystem crawl throughput,
SQLite refresh throughput, indexing-load latency, update lag and real/held-out
corpora still need measurements. M2 introduces daemon round-trip measurements.


### 2026-10-02: M1 review fixes (component-scoped parent matching)

Same reference environment, build flags and synthetic cases as the baseline
above. The query path changed: a word that misses the basename now matches
within the nearest matching parent directory name instead of across the full
path ([0007](adr/0007-partial-scans-and-component-parent-matching.md)).

| Paths | Build + scratch (s) | First query (ms) | Warm p50 (ms) | Warm p95 (ms) | Warm p99 (ms) | Peak RSS (KiB) |
|---:|---:|---:|---:|---:|---:|---:|
| 50,000 | 0.317 | 4.068 | 4.397 | 10.848 | 15.350 | 72,832 |
| 500,000 | 4.238 | 22.270 | 47.678 | 93.948 | 103.274 | 709,120 |

Interleaved reruns of the previous and new engines at 500k paths on the same
host measured p50 of about 37–42ms before and 48–50ms after; p95 stayed around
92–99ms for both. The extra cost is per-component scoring. Taking the best
component, rather than the nearest match, measured about 72–75ms p50 and was
rejected. The 5 ms p95 target remains **not met**. Raw output is in
[benchmark results](../tests/bench/results/2026-10-02-m1-fixes.txt).
