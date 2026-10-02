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
  lookup only matches complete tokens, so this measures how much the trigram
  channel covers before prefix typo correction is added (`partial_typo` below).

## Lexical benchmark (M1)

`make bench` (`tests/bench/`) builds the release engine and reports, per corpus:

- **Corpora.** A deterministic synthetic home folder (`corpus.c`: nested
  project/document folders from a ~130-word vocabulary, camelCase and separated
  names, numbered photos and screenshots, duplicate `README.md`/`index.js`, mixed
  extensions, Unicode fixtures) at 50k and 500k paths; and optionally a real
  NUL-separated path list (`BENCH_PATHS`, made with `scripts/make_corpus.sh`).
  Real lists may contain private names and are never committed.
- **Labeled queries** (`queries.c`): known-item queries generated from random
  ASCII-named targets, 200 per kind: `exact` basename; `prefix` (first ~60% of
  the stem); `abbreviation` (first letter plus consonants of each word,
  `prjnts`); `typo` (one random edit in the stem's longest word); `partial_typo`
  (an incomplete word with one edit, `projc`); `parent` (`<folder> <stem
  prefix>`). A result is relevant when it has the target's basename (duplicates
  count), and for `parent` also the same folder name. Seed 1 is the tuning set
  used while developing; seed 2 is held out and is the one reported.
- **Metrics:** Recall@1, Recall@10, MRR@10, and candidate recall
  (Recall@1000: the target survives candidate selection).
- **Latency:** every held-out query is typed one byte at a time and each
  keystroke is timed (the launcher workload, exercising caches and narrowing);
  then each whole query is timed after a different one. Engine build time and
  RSS are reported too. CLI startup and SQLite loading are excluded.
- **Fixtures:** nine sanity queries must pass on synthetic corpora
  (`README.md`, `read`, `prjnts`, `projectnotes.md`, `documents projectnotes`,
  `finance invoice2024`, `CAFÉ` (decomposed), `invoce2024`, a no-match query).

Prefix and partial-typo queries are often genuinely ambiguous (hundreds of
names start with `screens…`), so their known-item recall is bounded by the
corpus, not only by ranking.

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

### 2026-10-02: M1 completion (ADR 0008)

Same reference machine, build flags and library versions as above, but the host
was **heavily loaded** throughout (load average 5–9 on 12 threads, swap full,
28–37% iowait; a trivial loop over 500k array elements took 1.4–2.3 ms instead
of about 0.3 ms). Absolute latencies below are therefore pessimistic and noisy;
before/after comparisons were run back to back. Quality is the held-out seed.
Raw output: [benchmark results](../tests/bench/results/2026-10-02-m1-complete.txt).

**Latency and memory** (warm engine; typing = every keystroke of every held-out query):

| Corpus | Engine RSS | Typing p50 / p95 / p99 (ms) | Whole query p50 / p95 (ms) |
|---|---:|---:|---:|
| synthetic 50k, original engine | 81 MB | 12.7 / 26.7 / 35.8 | 10.3 / 23.7 |
| synthetic 50k | 30 MB | **0.05 / 1.05 / 2.07** | 0.66 / 2.02 |
| synthetic 500k | 230 MB | 0.80 / 15.9 / 25.7 | 9.5 / 25.4 |
| real `/usr` 500k (58 B/path) | 226 MB | 0.23 / 6.7 / 16.3 | 4.1 / 15.6 |

For reference, the first baseline's peak RSS at 500k was 709 MB (uniform
synthetic corpus). The **5 ms p95 gate is met at 50k and not at 500k.** Most
keystrokes at 500k are well under a millisecond: one-symbol queries come from
the seal-time cache, later keystrokes narrow, and strong prefix hits skip scans.
The p95 comes from keystrokes that need a full scan of a common-letter word
(4–15 ms here), above all the first letter of a second word when the first word
matches a large share of the corpus through folder names (`apps o`: 123k icons
under `apps/` folders). The synthetic corpus is harsher than the real one
because its ~130-word vocabulary repeats in folder names. Next options:
intra-query parallel scans in the M2 daemon, a byte-level ASCII name layout, and
re-measuring on an unloaded machine.

**Ranking quality** (held-out; R@1 / R@10 / MRR@10 / candidate recall@1000):

| Kind | synthetic 50k, original | synthetic 50k | real `/usr` 500k |
|---|---:|---:|---:|
| exact | .97 / 1.0 / .98 / 1.0 | .97 / 1.0 / .98 / 1.0 | .99 / 1.0 / .99 / 1.0 |
| prefix | .27 / .44 / .33 / 1.0 | .25 / .43 / .31 / 1.0 | .47 / .71 / .54 / .99 |
| abbreviation | .43 / .70 / .52 / .97 | .48 / .73 / .55 / .97 | .44 / .61 / .48 / .98 |
| typo | .21 / .26 / .23 / .32 | **.76 / .85 / .79 / .99** | .78 / .91 / .83 / .98 |
| partial typo | .01 / .02 / .01 / .35 | .02 / .04 / .02 / .57 | .06 / .14 / .08 / .46 |
| parent | .30 / .59 / .39 / .99 | .28 / .65 / .37 / 1.0 | .29 / .48 / .33 / .91 |
| **all** | .36 / .50 / .41 / .77 | .46 / .62 / .50 / .92 | .50 / .64 / .54 / .88 |

Typo correction is the main quality gain. Prefix and partial-typo queries are
often ambiguous (many names share the fragment), which bounds known-item recall.
Partial typos stay weak, because typo lookup only corrects complete tokens:
prefix typo correction remains a later extension. Raising the parent-folder
prefix tier (ADR 0008) lifted real parent Recall@10 from 0.355 to 0.475 without
lowering other kinds. All nine synthetic fixtures pass at 50k and 500k.

### 2026-10-02: M2 resident snapshot foundation (ADR 0010)

Same reference machine and release flags. `make bench` now also transfers the
sealed engine into a catalog snapshot with one preallocated reader workspace and
times acquisition, whole-query search, and release over the 1,200 held-out
queries. Construction/publication, SQLite, IPC, and concurrent indexing are
excluded. The direct and leased measurements are sequential runs with different
initial workspace cache contents; their difference is not an isolated estimate
of mutex overhead. The daemon is not implemented yet.

| Paths | Build + scratch (s) | Typing p50 / p95 / p99 (ms) | Direct whole query p95 (ms) | Leased whole query p50 / p95 / p99 (ms) | Peak RSS (KiB) |
|---:|---:|---:|---:|---:|---:|
| 50,000 | 0.294 | 0.055 / 1.043 / 1.988 | 1.827 | 0.703 / 2.010 / 3.077 | 44,332 |
| 500,000 | 3.822 | 0.953 / 16.686 / 25.163 | 22.406 | 9.577 / 23.964 / 32.333 | 392,392 |

All nine synthetic fixtures pass at both sizes. Held-out all-kind Recall@10 is
0.615 at 50k and 0.531 at 500k; candidate recall@1000 is 0.920 and 0.825.
Ranking is unchanged. Peak RSS covers the entire benchmark process, including
corpus/build allocations; it does not measure multiple retained snapshots or
daemon steady-state memory. The 5 ms p95 gate remains unmet at 500k.
Raw output: [M2 snapshot benchmark](../tests/bench/results/2026-10-02-m2-snapshots.txt).

ASan/UBSan tests additionally cover four readers pinned through 24 publications,
acquisition/query/release racing publication and reclamation, reader/view
capacity exhaustion, and a separate WAL writer committing during catalog load.
These establish lifecycle behavior, not update throughput or daemon round-trip
latency. M2 still needs socket, writer/history, filesystem-watch and recovery
integration acceptance tests.

### 2026-10-02: M2 resident daemon (ADR 0011)

`make bench-daemon` runs the release daemon with the M1 synthetic corpus (seed 42)
and the same 1,200 held-out queries (seed 2). Fixture SQL deduplicates paths and
stores them under an unavailable synthetic root; the daemon retains those saved
rows. A separate real root has 100 files, then receives 101 new files to measure
reconciliation/publication while queries run. This measures real socket traffic
and whole-engine rebuild cost, not crawling 500k physical files. History is
disabled. One persistent client requests ten results at a time; each whole query
is sent independently. It does not measure typing-prefix latency.

Reference: Intel Core i7-8700K, 32,033 MiB RAM, Linux 6.8.0-139, C17 `-O3
-DNDEBUG`; load average started at 2.12/2.65/3.02. Warm engine timing is the
daemon's `timing.engine_us` around lexical search alone. Round-trip timing starts
before send and ends after receiving the terminal newline, including queue wait,
leases, encoding and sockets (Python JSON parsing follows the timer).

| Requested / actual catalog paths | Startup (ms) | First query round trip (ms) | Warm engine p50 / p95 / p99 (ms) | Warm round trip p50 / p95 / p99 (ms) |
|---:|---:|---:|---:|---:|
| 50k / 49,569 | 306 | 0.706 | 0.604 / 1.578 / 2.847 | 0.688 / 1.733 / 3.045 |
| 500k / 494,362 | 3,275 | 8.437 | 8.367 / 19.113 / 26.550 | 8.485 / 19.282 / 26.792 |

| Paths | Round trip during rebuild p50 / p95 / p99 (ms) | Update publication lag (ms) | Update reconciliation (ms) | Initial RSS / peak RSS during update (KiB) |
|---:|---:|---:|---:|---:|
| ~50k | 0.589 / 1.470 / 2.130 (254 samples) | 402 | 301 | 31,876 / 71,084 |
| ~500k | 6.679 / 20.048 / 37.943 (264 samples) | 3,705 | 3,592 | 250,720 / 641,176 |

The empty/small daemon's initial 100-file crawl/SQL/build cycle took 3–4 ms.
Large saved-catalog startup includes SQLite load, engine construction and scratch;
it does not wait for a full filesystem crawl. Update lag includes coalescing,
scan, private build, commit, publication and the observing query. RSS comes from
the daemon's `/proc` status; peak includes old/staged engines and builder buffers.
After update, RSS was 53,424 / 552,036 KiB at the two sizes; allocator retention
means reclamation does not necessarily return pages to the OS immediately.

The **5 ms p95 target remains unmet at ~500k**. Full rebuilds also make a small
update take about 3.7 seconds there and raise peak RSS to about 626 MiB. Shared
index blocks/incremental updates and lexical scan improvements remain performance
work; M2's functional acceptance does not claim those targets were reached.
Raw output: [daemon benchmark](../tests/bench/results/2026-10-02-m2-daemon.txt).

M2 acceptance now includes sanitizer integration for create/delete/rename,
directory moves, byte paths/newlines, exact ids, concurrent queries/updates,
SQLite lock isolation, failed-write rollback, unreadable/offline scopes,
watch exhaustion, restart repair, asynchronous/deduplicated/disabled history,
malformed and oversized inputs, cancelled/duplicate ids and client deadlines.
Unit fault injection validates old-view service after committed publication
failure, gating later catalog batches, history saturation and deterministic
overflow reconciliation. The regular lexical benchmark remains separate from
socket timing; its output is recorded in
[M2 lexical check](../tests/bench/results/2026-10-02-m2-complete-lexical.txt).
