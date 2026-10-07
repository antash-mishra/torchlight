# M6 plan: responsive search and incremental indexing

M1 to M4 are implemented. M4's acceptance gates (500k final-phase latency and
broader relevance) stay open and are measured again at the end of M6. This
document is the working plan for M6. It refines the three steps in
[ADR 0026](adr/0026-search-workers-simd-and-incremental-indexing.md) using a
code survey and a profile of the current engine taken on 2026-10-07.

## In simple words

Today one thread in the daemon reads socket requests, runs the whole search,
and writes the answer. While it searches, it cannot read the next keystroke.
A second thread does everything else: watches the filesystem, rescans every
root when anything changes, rebuilds the whole search engine from scratch, and
saves history in between. At 500k paths a single touched file costs a 4 to 5
second rescan-and-rebuild and briefly doubles memory.

After M6:

- The socket thread only reads requests and writes replies. A search thread
  does the searching, always on the newest query a client sent, and drops work
  for queries the user has already typed past.
- Scoring uses Frizbee's SIMD matcher, and the engine looks at fewer candidate
  entries per query, so a search finishes faster.
- A changed file updates only the entries in the affected folders. The engine
  keeps its big immutable base and adds a small delta on top; a background
  compaction merges them now and then. Full rebuilds remain for recovery.
- History and catalog commits go through one persistence owner that never
  scans the filesystem or builds indexes inside a database transaction.

## Where the code is today

| Area | Today | Pointer |
|---|---|---|
| Query dispatch | Lexical search runs inline on the poll thread | `src/service/daemon.c` `execute` → `catalog_query` |
| Supersession | Only a newer query already in the same receive buffer cancels the current one; scoring never checks a cancel flag | `daemon.c` `newer_query` |
| Semantic phase | Own worker, per-client token, stale results dropped | `src/service/semantic.c` `semantic_take` |
| Indexing | Any change: full crawl of all roots, load all rows, `lexical_finish` rebuilds every index | `src/service/writer.c` `reconcile`, `make_snapshot` |
| History | Drained on the writer thread, blocked during a scan | `writer.c` `drain_history` |
| Watch → writer | Events only set a dirty flag plus rename pairs; affected directories are not kept | `writer.c` `changed` |
| Semantic snapshot | Re-copies every path on each catalog publish | `semantic.c` `copy_files` |
| Catalog snapshots | Capacity 2; the writer backs off when both are live | `src/index/catalog.c` `catalog_publish` |
| Scoring | C dynamic-programming scorer, 512-symbol optimal bound, greedy fallback | `src/index/fuzzy.c` |

### Baseline to beat (500k synthetic paths, i7-8700K, AVX2, 12 threads)

| Measurement | Value | Source |
|---|---|---|
| Engine typing p95 / whole-query p95 | 8.55 ms / 9.62 ms | `tests/bench/results/2026-10-05-m4-native-lexical.txt` |
| Daemon round trip p95, lexical only | 7.27 ms | `2026-10-03-m3-daemon.txt` |
| Daemon lexical-phase p95, hybrid | 12.7 ms | `2026-10-05-m4-hybrid-daemon.jsonl` |
| Update lag, 100 touched files | 4.4 to 5.0 s | both daemon runs |
| RSS before → after rebuild, lexical only | 275 → 549 MB, peak 661 MB | `2026-10-03-m3-daemon.txt` |
| RSS before → after rebuild, hybrid | 590 → 802 MB, peak 972 MB | `2026-10-05-m4-hybrid-daemon.jsonl` |
| Engine build time | 3.6 s of the 4.4 s lag | `bench_lexical` `build_s` |

### Where query time goes (gprof, 500k, 17k queries)

| Share | Work | Per query |
|---|---|---|
| ~46% | `fuzzy_score`, `entry_score`, result heap | ~2.6k fuzzy scores, ~5.8k entry scores |
| ~12% | prefix-hit callbacks | ~18k hits |
| ~11% | directory ancestor scoring (`dirtree_parent`, `own_score`) | ~41k ancestor visits |
| ~7% | trigram lookup | |
| ~24% | query setup, mask index, typo hits, filtering | |

Conclusion: a faster scorer alone caps the gain near 1.4x (about 7 ms p95).
The 5 ms target also needs fewer candidates per query. Step 2 therefore pairs
Frizbee with candidate-volume work.

## Target flow

```
                     torchlightd (one process)
 GTK popup / CLI
   │ query q1, q2, q3 ...        ┌──────────────────────────────────────────┐
   ▼                             │ IPC thread (poll loop)                   │
 Unix socket ───────────────────►│  read request → put in client's          │
   ▲                             │  newest-pending slot (q3 replaces q2)    │
   │ reply for q3 only           │  write replies, drop stale completions   │
   │                             └───────┬──────────────────────▲───────────┘
   │                                     │ wake                 │ done(q3)
   │                             ┌───────▼──────────────────────┴───────────┐
   │                             │ Search thread                            │
   │                             │  take newest pending per client          │
   │                             │  lease catalog + desktop snapshot        │
   │                             │  prefix/trigram/subseq/typo → candidates │
   │                             │  Frizbee score batches (cancel check     │
   │                             │  between batches) → rank → results       │
   │                             │  then hand off to semantic worker        │
   │                             └───────────────▲──────────────────────────┘
   │                                             │ immutable snapshots
   │              ┌──────────────────────────────┴───────────────────────┐
   │              │ Catalog: base segment + delta segment, catalog_gen N  │
   │              │ (readers lease N; publish N+1 when commit succeeds)   │
   │              └──────────────▲───────────────────────────────────────┘
   │                             │ publish
   │   inotify  ┌────────────────┴───────────┐    ┌──────────────────────┐
   └──events───►│ Indexing thread            │    │ Persistence thread   │
                │  coalesce → affected dirs  │    │  owns SQLite         │
                │  scoped crawl of those dirs│───►│  commits catalog     │
                │  build delta privately     │◄───│  batch, then signals │
                │  compaction in background  │    │  publish; drains     │
                │  full rebuild for recovery │    │  history ring        │
                └────────────────────────────┘    └──────────────────────┘
                                                  ┌──────────────────────┐
                                                  │ Semantic worker      │
                                                  │  (M4, unchanged role)│
                                                  │  takes delta updates │
                                                  │  instead of recopy   │
                                                  └──────────────────────┘
```

Rules the diagram encodes:

1. Only the IPC thread touches sockets. Only the search thread runs lexical
   queries. Only the persistence thread writes SQLite.
2. A client has one pending query. A newer query replaces it before the search
   thread starts, and sets a cancel flag if the search already started. The
   IPC thread drops a completion whose request id is no longer the client's
   newest.
3. Readers lease an immutable `catalog_gen`. Indexing never mutates a live
   snapshot; it builds a delta and publishes a new gen after the commit.
4. Filesystem scans and index construction happen outside database write
   transactions.

## Steps

### Step 1: search thread and persistence owner

**Status: implemented** (2026-10-07), see
[ADR 0028](adr/0028-m6-search-thread-and-persistence-owner.md). The recorded
step-1 run is referenced from the measurements section below.

Goal: the IPC loop never blocks on a search, a scan or a history write.

- Add a search thread owned by `tl_daemon`. Move `execute` and
  `catalog_query` off the poll loop. Keep response encoding on the IPC
  thread, or encode on the search thread into a per-client buffer that the IPC
  thread flushes; either way sockets never hold a workspace lease.
- Replace `newer_query`'s buffer scan with a newest-pending slot per client
  plus a `cancelled` flag. Thread the flag into `lexical_query` as a
  cancellation callback checked in `score_batch` and the scan loops. A
  cancelled query returns `TL_STATE` with reason `superseded`, as today.
- Suppress stale completions by request id on the IPC thread. Keep the
  semantic token/deadline path as it is; the search thread calls
  `semantic_submit` after the lexical results are ready.
- Split `tl_writer` into an indexing worker (watch, crawl, engine build) and a
  persistence worker (SQLite, history ring, retention). The persistence worker
  is the only `tl_store` owner on the catalog side. Indexing prepares a batch
  and hands it over; persistence commits and reports the new `catalog_gen`;
  indexing publishes. `drain_history` moves to the persistence worker so a
  scan no longer blocks history.
- Raise catalog snapshot capacity, or release leases per batch, so a search
  thread that holds leases longer cannot starve publication (`catalog_publish`
  fails at capacity 2 today).
- Preserve: duplicate-id rejection, `DAEMON_ACTIVE_REQUESTS`, desktop and
  file snapshot coherence, shutdown that joins all threads, and M4's cache
  retry contracts.

Exit criteria: typing a 10-character query as 10 rapid requests yields one
completed search for the last request and `superseded` for the rest, with no
backlog; history counters keep moving during a 500k scan; sanitizer tests for
the new threads; `bench-daemon` shows IPC wait (round trip minus engine) no
higher than today. Latency p95 is not expected to improve in this step.

### Step 2: Frizbee scoring and fewer candidates

Goal: warm lexical p95 below 5 ms at 500k.

Frizbee integration (the library choice is made; this is the wrapper work):

- Vendor the `frizbee/frizbee.h` header and pin the release tag (C bindings
  exist from v0.13.0). Build the static library with `cargo cbuild` through a
  Makefile rule, or vendor the prebuilt x86_64-linux SDK; decide once and
  document it in `docs/modules/fuzzy/README.md`.
- Input: Frizbee needs UTF-8 and does no normalization or diacritic folding.
  Keep `tokenize` as the normalizer and give Frizbee normalized UTF-8 text.
  Non-UTF-8 paths and strings over the optimal bound keep the current C
  scorer. Measure the extra memory of storing normalized text.
- Score scale: re-derive `fuzzy_score_bound`, the score-band static asserts,
  exact-name priority and field weights on Frizbee's scale. Re-baseline the
  labeled fixture with a stated tolerance instead of expecting identical
  numbers.
- Allocation: Frizbee allocates its result list. Use a caller-buffer entry
  point if the C ABI offers one; otherwise record a bounded exception to the
  allocation-free query rule and keep it out of the per-entry path.
- SIMD: the reference machine has AVX2 only. Decide between padded buffers and
  Frizbee's `safe_read` mode, and record the choice with the benchmark.

Candidate volume (needed to reach the target, see the profile):

- Bound prefix fan-out: short prefixes currently produce ~18k hits per query.
  Resolve short-prefix queries through `symbol_results` or capped postings
  before scoring.
- Score directory ancestors once per directory, not once per entry
  (~41k `dirtree_parent` visits per query today).
- Keep the heap but push only entries that can beat the current k-th score
  using the new bound.
- Optional spike, time-boxed: Frizbee's list mode over all basenames with its
  SIMD prefilter, as a replacement for channel retrieval on single-word
  queries. Only adopt it if recall on the labeled fixture holds; the channel
  contracts in ADR 0026 remain the default.

Exit criteria: `make bench` 500k typing p95 below 5 ms on the reference
machine; held-out recall@10 and candidate recall within the stated tolerance;
raw-byte path tests, exact-match and typo tests pass; `make lint` clean with
the vendored header.

### Step 3: incremental indexing

Goal: a small change publishes in well under a second at 500k without a full
scan or rebuild, and does not double RSS.

Three sub-steps, each shippable:

- **3a. Scoped reconcile.** `watch` already coalesces events; pass the set of
  affected directories (plus rename pairs) to the indexing worker instead of
  a dirty flag. Crawl only those directories, upsert and prune only their
  entries, and skip the full `store_load`. Overflow, unavailable watches and
  periodic rescan keep the full path. Expected effect: removes the crawl share
  of the lag; the rebuild share (3.6 s) remains.
- **3b. Segmented engine.** Keep the published engine as an immutable base
  segment and build a small delta segment for changed entries, with a
  tombstone set for removed or replaced ids. `lexical_query` runs over base
  plus delta and filters tombstones; `rank` merges. Compaction rebuilds a new
  base from base plus delta in the background when the delta exceeds a size
  bound, then publishes it. Unchanged structures (prefix, trigram, typo,
  dirtree, mask index) are shared, not copied. This is the piece that hits the
  lag and peak-RSS targets.
- **3c. Incremental semantic snapshot.** `copy_files` restages all paths on
  every publish. Give the semantic worker the same delta (added, removed,
  changed ids) so a small publish updates its metadata snapshot in place and
  only re-embeds changed paths. Keep `emb_gen` versioning and model
  replacement unchanged.

Preserve: commit before publication, incarnation and rename identity,
successful-scope deletion only, overflow reconciliation, restart recovery and
full rebuild as the recovery and compaction path.

Exit criteria: `bench-daemon` update lag for 100 touched files below 500 ms at
500k; peak RSS during a small update within 10% of steady RSS; catalog crash
tests and rename tests pass against the segmented engine; compaction runs
without a query stall.

## Step 1 recorded run (2026-10-07)

Lexical-only daemon and engine benchmarks after step 1, same synthetic corpus
and machine as the baseline, with a load average near 6 from a preceding lint
run. Step 1 changes responsiveness, not search cost, and the numbers agree:

| Measurement at 500k | Baseline | Step 1 | Source |
|---|---|---|---|
| Daemon round trip p95 | 7.27 ms | 7.30 ms | `2026-10-07-m6-step1-daemon.jsonl` |
| Daemon engine p95 | 6.93 ms | 7.06 ms | same |
| Round trip p95 during indexing | 10.8 ms | 8.17 ms | same |
| Update lag, 100 touched files | 4.97 s | 4.03 s | same |
| RSS before → after rebuild | 275 → 549 MB, peak 661 MB | 277 → 497 MB, peak 677 MB | same |
| Engine typing p95 / whole-query p95 | 8.55 / 9.62 ms | 7.36 / 7.33 ms | `2026-10-07-m6-step1-lexical.txt` |

The engine-only improvement is within run-to-run variance of this machine and
is not claimed as a step-1 effect; the cancellation polls cost nothing
measurable. The peak-RSS increase is the private crawl batch; step 3a removes
it for routine updates. A live check against a real 35k-entry workspace folder
answered every query under 1.5 ms round trip, answered a four-frame typing
burst in 0.3 ms with three `cancelled/superseded` replies and one result, and
served popup-style one-connection-per-keystroke typing at 0.1 to 0.4 ms per key.

## Measurements recorded at the end of M6

Engine and IPC p50/p95/p99 at 50k and 500k, latency during indexing, update
lag, history queue counters, steady and peak RSS, and the M4 final-phase p95,
all written to `tests/bench/results/` with the machine description. The M4
acceptance gate is re-evaluated from these numbers.

## Decisions (recorded 2026-10-07)

1. **Step 2 shape:** scorer swap plus candidate-volume work, with a time-boxed
   spike (about two days) of Frizbee's list mode. Adopt the spike only if the
   labeled fixture's recall holds.
2. **Frizbee build:** use Frizbee's own C binding (`bindings/frizbee-c`) and
   compile it from a pinned release tag with `cargo cbuild` from a Makefile
   rule. Rust is therefore a build-time dependency; the prebuilt SDK stays as
   a fallback for machines without a Rust toolchain.
3. **Step 3 delivery:** three shippable sub-steps (3a, 3b, 3c), each with its
   own tests and benchmark run.

Still open: the delta size bound and compaction trigger for 3b, to be chosen
from measurements during 3a.

## Out of scope

Personalization (M5), document-content search, a mapped read-only index file
and Wayland work. Nothing here changes the IPC protocol seen by the popup.
