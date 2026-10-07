# Evaluation

How Torchlight's search quality and performance are measured. Every milestone
validates against this list; performance numbers in `PLAN.md` are targets until
recorded here.

Current M1/M2 acceptance and its measurements are in the
[M3 readiness report](m3-readiness.md) and
[3 October readiness results](#2026-10-03-m1m2-readiness-adr-0013).
The latest ranking regression and benchmark are in the
[application-name check](#2026-10-05-application-name-ranking-adr-0018).
The [M4 foundation measurements](m4-implementation.md#initial-measurements)
record the exhaustive float cosine reference and RRF cost at 50k/500k synthetic
vectors. These exclude inference and are not model relevance or hybrid latency.

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

### 2026-10-05: application-name ranking (ADR 0018)

The running catalog returned Google Chrome 25th for `chrome`, outside the popup's
ten results. [ADR 0018](adr/0018-application-name-ranking.md) adds bounded name
prefix weighting in the desktop engine and preserves its tie ordering during
merging. Sanitizer integration checks use 24 competing filename prefixes, an
unrelated keyword-only application and twelve tied application entries. They
cover typing, case, multiword names, result limits, and exact file/path priority.
An isolated sanitizer daemon also checked the actual installed
`google-chrome.desktop` against 24 files: Chrome was first at limits 1/10/1,000.
That isolated check left the user's daemon unchanged. After the requested
restart, the live device's saved catalog also returns Google Chrome first at
limits 1/10/1,000. The replacement runs the verified updated executable with the
original arguments, environment, configuration and database.

`make test` and `make lint` pass. `make bench` retains all nine synthetic fixtures
at 50k/500k, with unchanged held-out file-ranking metrics (the default file prefix
bonus is zero). The same i7-8700K, Linux Mint reference machine and C17 `-O3
-DNDEBUG` benchmark were used. These are warm synthetic engine measurements;
they do not measure hybrid search or prove broad application relevance.

| Paths | Typing p50 / p95 / p99 (ms) | Leased whole-query p50 / p95 / p99 (ms) | Peak RSS (KiB) |
|---:|---:|---:|---:|
| 50,000 | 0.046 / 0.658 / 1.513 | 0.128 / 0.806 / 1.417 | 46,440 |
| 500,000 | 1.179 / 7.169 / 12.404 | 1.748 / 7.332 / 13.429 | 399,204 |

The original 5 ms p95 target remains unmet. The independent one-file test of
`proej` for `projectNotes.md` still misses; partial typo retrieval is not fixed by
the application-name weighting. [The search quality review](search-quality.md)
proposes broader relevance comparisons and newer M4 model candidates, without
claiming a selected or implemented replacement.
Raw output: [application-name ranking benchmark](../tests/bench/results/2026-10-05-application-name-ranking.txt).

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

### 2026-10-03: M2 review regressions (ADR 0012)

The [two reproduced M2 findings](m2-review.md) are fixed. ASan/UBSan module and
integration coverage now includes coalesced file/directory replacement,
same-inode/different-birth identity, retirement of descendant ids, failed-write
rollback/retry with open-history restoration, replacement during downtime,
schema-v1 migration, periodic scans with no inotify instance, restored watch
coverage and retention of the previous watch set on a later factory failure.
Clang-tidy and cppcheck pass.

Rerunning the original reproductions with the fixed sanitizer build gives a
fresh replacement id and `stale_result` for the old id. With inotify_init1
forced to fail, the new file is indexed, reconciliations continue, and status
reports degraded/unavailable watching. No query-path change was made; recorded
release performance tables above remain the pre-fix measurements.

### 2026-10-03: M1/M2 readiness (ADR 0013)

Same reference machine, compiler, release flags and library versions as above.
The shared host was unpinned; lexical load average started at 2.56/2.60/2.28,
daemon load at 1.72/2.44/2.27. No semantic model applies. Optimization used the
seed-1 tuning workload; the regular latency and held-out quality runs still
use seed 2 and the seed-42 synthetic corpus. The query path now uses complete
resident mask filtering, sequential directory scoring, four bounded word
evidence caches and three prestarted workers per large reader workspace.

`make bench`:

| Paths / mean bytes | Build + scratch (s) | Engine RSS (KiB) | Typing p50 / p95 / p99 (ms) | Whole-query p50 / p95 / p99 (ms) | Leased p50 / p95 / p99 (ms) | Peak process RSS (KiB) |
|---:|---:|---:|---:|---:|---:|---:|
| 50,000 / 71.4 | 0.237 | 27,896 | 0.041 / 0.518 / 1.078 | 0.127 / 0.701 / 1.249 | 0.121 / 0.676 / 1.149 | 44,364 |
| 500,000 / 79.6 | 2.783 | 255,912 | 0.992 / 5.970 / 9.579 | 1.561 / 6.154 / 8.956 | 1.467 / 6.013 / 9.223 | 396,972 |

Typing includes 12,406 / 12,477 samples; whole and leased runs each include
1,200 queries. Warm timing excludes startup, SQL and construction. Engine RSS
is measured after construction/scratch; process peak includes corpus/build and
later benchmark resources. It does not describe daemon snapshot-update memory.
Held-out all-kind Recall@1 / Recall@10 / MRR@10 / candidate Recall@1000 is
0.457 / 0.615 / 0.504 / 0.920 at 50k and
0.362 / 0.531 / 0.414 / 0.825 at 500k, unchanged from the previous engine.
All nine synthetic fixtures pass at both sizes. Raw output:
[lexical benchmark](../tests/bench/results/2026-10-03-m3-readiness-lexical.txt).

`make bench-daemon`, with the same saved synthetic catalog and separate physical
101-file update workload described for ADR 0011 above:

| Requested / actual paths | Startup (ms) | First round trip (ms) | Warm engine p50 / p95 / p99 (ms) | Warm round trip p50 / p95 / p99 (ms) |
|---:|---:|---:|---:|---:|
| 50k / 49,569 | 264.459 | 0.243 | 0.140 / 0.858 / 1.452 | 0.233 / 0.964 / 1.571 |
| 500k / 494,362 | 3,043.261 | 1.059 | 1.395 / 5.615 / 8.328 | 1.521 / 5.706 / 8.467 |

| Paths | Round trip during rebuild p50 / p95 / p99 (ms) | Update lag (ms) | Reconciliation (ms) | Initial RSS / peak update RSS (KiB) |
|---:|---:|---:|---:|---:|
| ~50k | 0.190 / 0.966 / 2.387 (596 samples) | 395.950 | 295 | 33,756 / 78,888 |
| ~500k | 1.705 / 6.776 / 10.161 (1,224 samples) | 4,061.917 | 3,959 | 272,932 / 657,936 |

The initial physical 100-file reconciliation took 4 ms at both sizes. After
update, daemon RSS was 61,272 / 546,100 KiB; allocator retention can keep pages
resident after reclaim. Warm daemon query timing and socket round trips are
separate measurements. History is disabled, and this is not a large physical
crawl measurement. Raw output:
[daemon benchmark](../tests/bench/results/2026-10-03-m3-readiness-daemon.txt).

The user accepts **roughly 6 ms p95 to advance to M3** and defers optimization
toward the original 5 ms target until the whole system is built. Full rebuilds
still cost roughly four seconds and 642.5 MiB peak update RSS near 500k; smaller
incremental updates remain later work. Unfinished misspelled-token relevance
also remains weak. Neither is claimed fixed by this query change.

`make test` passes ASan/UBSan/leak checks, independent scalar bitmap checks,
analytic worker-batch scores, exact-name and evidence-eviction regressions,
allocator interposition (long queries and active 100k workers), and CLI/daemon
integration. `make lint` passes clang-tidy and cppcheck with zero warnings.
A differential development check matches ordered ids/scores against the prior
algorithm with the same typo-bound fix over 6,026 typing/backspace cases at 50k
and 6,082 at 500k, including capacities ten and 1,000. This supplements the
independent score fixtures; it is not a relevance metric. ThreadSanitizer could
not start on this host (`unexpected memory mapping`), so it provides no result.

## M3 verification (3 October 2026)

M3 adds desktop discovery, mixed results and the GTK popup. The
[M3 verification report](m3-completion.md) records unit/integration checks,
Cinnamon focus/theme/scale observations, native settings activation and remaining
platform coverage. Raw file-corpus benchmarks are
[lexical](../tests/bench/results/2026-10-03-m3-lexical.txt) and
[daemon](../tests/bench/results/2026-10-03-m3-daemon.txt). XDG applications are
isolated in the file benchmark; mixed catalog behavior has a separate integration
suite. At 500k the recorded engine/IPC p95 was 6.929/7.265 ms and update lag
4.973 s, with concurrent validation activity. File ranking and 9/9 fixtures are
unchanged. The earlier roughly 6 ms readiness acceptance and deferred 5 ms target
remain the performance context, not a claim that this new run meets 5 ms.

## M3 Part 2 labeled relevance

The [mixed-catalog fixture and evaluator](../tests/quality/README.md) supplement
historical synthetic known-item benchmarks with realistic app/settings names,
competing files and ambiguous graded labels. The split keeps target families
and their variants together. Native runs compare the frozen pre-change engine
against explicit fields, prefix editing and optimal alignment; an independent
exact-token field BM25 baseline is also recorded. Cold-workspace and result-limit
checks run for every labeled query. At fewer than 1000 entries the fixture returns
complete matching membership, so candidate recall is exact rather than a top-k
proxy. Per-class first-useful rank, MRR, top-ten success, nDCG@10 and no-match
accuracy are stored in the JSON artifact.

[Part 2 verification](m3-part2-completion.md) reports held-out results, 50k/500k
warm typing/whole queries, fresh-workspace query timing and steady/peak RSS.
The fresh-workspace timing excludes allocation/worker startup and is not an OS
cold-cache measurement. Prefix edits and optimal alignment increase work;
the original 5 ms target remains separate optimization work. Earlier statements
above about missing prefix correction describe the historical M1 baseline.

## M4 native model and hybrid evaluation

[Model evaluation](m4-model-evaluation.md) records pinned English Potion research,
small target-family splits, native parity, quantized/approximate reference recall,
two-phase daemon latency/RSS and the private Documents aggregate smoke check.
M4 functionality is opt-in; 500k latency and broader model/relevance acceptance
remain open. Normal sanitizer tests require no downloaded model.

## M6 step 1: search thread and persistence owner (2026-10-07)

Step 1 moves search onto a dedicated thread with per-client request queues and
cooperative cancellation, and splits the writer into indexing and persistence
threads. It is a responsiveness change, so the acceptance check is that search
cost is unchanged and typing bursts leave no obsolete backlog. Recorded runs:
[daemon benchmark](../tests/bench/results/2026-10-07-m6-step1-daemon.jsonl)
and [lexical benchmark](../tests/bench/results/2026-10-07-m6-step1-lexical.txt);
the comparison table is in the [M6 plan](m6-plan.md). Regression coverage adds
rapid typing with interleaved status frames, abandoned connections, engine
cancellation and history draining while publication is blocked.
