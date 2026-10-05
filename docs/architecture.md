# Architecture

## Current implementation

M3 runs as a resident file/application daemon with a GTK4 popup. `torchlight query` uses Unix-socket IPC;
explicit `--db` retains the M1 local query mode. Offline `index` still wires
config -> crawl -> store, under the same database singleton lock as the daemon.
The poll loop searches immutable catalog leases, encodes one terminal response
and releases the lease before sending. Its worker owns SQLite, inotify,
reconciliation, full-engine staging/publication and asynchronous history.
Saved entries serve before background reconciliation. Status and restart/failure
recovery are implemented. Semantics and personalization ranking below remain the target architecture. See ADR 0011 for the full-rebuild baseline and
structural bounds. Roughly 6 ms p95 at 500k paths is accepted for starting M3;
the original 5 ms target is later optimization work. See
[readiness](m3-readiness.md) and the [GUI specification](m3-gui-design.md).

M3 Part 2 adds explicit generic-name/keyword fields beside primary names and
filesystem folders, indexed unfinished-word one-edit matching through sorted
term ranges, complete-token scoring and optimal fuzzy alignment up to 512
symbols. Queries remain allocation-free; the large-text greedy fallback and
all existing capacity/cache contracts remain bounded. See
[ADR 0020](adr/0020-m3-part2-search-quality.md) and
[measured relevance/performance](m3-part2-completion.md). M4 has started with
an owned float cosine reference, swappable embedder adapter and bounded RRF
fusion. These modules are not yet wired into the daemon; trained inference,
persisted/background vectors and coherent two-phase snapshots remain pending.
See [M4 implementation](m4-implementation.md) and
[ADR 0022](adr/0022-m4-semantic-foundation.md). M5 optional personalization follows.

ADR 0012 adds filesystem incarnation checks during scans and schema v2 identity
storage. Replacements retire old ids and descendants before publication, with
rollback restoring history. Inotify instance failure degrades watch coverage
while periodic/explicit reconciliation continues and retries setup.

ADR 0013 adds complete resident bitmap filtering, four per-query word evidence
caches and bounded scoring workers. Large batches resolve directory evidence
before dispatch; workers write disjoint outputs and the coordinator orders
results. Query evaluation retains the same complete matching and ranking rules.

## Components

```
┌─────────────┐   Unix socket   ┌──────────────────────────────────────┐
│ GTK4 / CLI  │ ◄─────────────► │  torchlightd                         │
└─────────────┘  query/results  │  ├─ crawl + watch   (fs)             │
                                │  ├─ lexical channels → fuzzy/edit    │
                                │  ├─ embed → vector                   │
                                │  ├─ rank (RRF + personalization)     │
                                │  └─ store (SQLite, WAL)              │
                                └──────────────────────────────────────┘
```

## Dependency direction

```
bin/, ui/  →  service/  →  ipc/, fs/, storage/, index/  →  core/
```

Modules only depend downward. `core/` depends on nothing in Torchlight.
IPC clients can also be used directly by bin/UI callers. Service orchestration
depends on the lower modules; filesystem and index modules never call storage.

## Indexing flow

1. `crawl` walks roots and emits path/stat data with device/inode and birth time
   (ctime fallback). `store` retires changed incarnations before upserting.
2. The writer validates catalog changes and prepares private index deltas.
3. `tokenize` normalizes raw path bytes and retains boundary metadata. `prefix`,
   `trigram`, `subseq` (character masks) and `typo` build their structures privately.
4. `store` commits the catalog batch, then the writer publishes `catalog_gen`.
   Queries pin that `catalog_gen`; old blocks remain live until readers release them.
   Failed post-commit publication triggers reload/rebuild before later updates.
5. A background thread embeds changed paths. The writer validates path/model
   versions, persists vectors, and publishes corresponding vector updates.
6. `watch` coalesces incremental events. Overflow, startup, and unavailable
   watches trigger reconciliation; unsuccessful scans cannot establish deletion.

Install watches during crawling and reconcile affected directories afterward.
Paired renames preserve ids; directory moves update descendants and invalidate
their path embeddings. Model replacement uses staging storage and activates a
validated embedding generation (`emb_gen`) together, without mixing model versions.

## Query flow (targets: lexical phase p95 < 5ms, final phase p95 < 10ms)

1. The client sends a bounded, versioned JSON-line query with a request id.
2. Assign a search id in memory and enqueue optional history once. Pin a
   `catalog_gen`/`emb_gen` pair. **Lexical:** run `prefix`,
`subseq`, and `trigram`,
   plus eligible one-edit `typo` lookup unless an exact full-basename match
   makes it unnecessary. One/two-character queries use `prefix` and `subseq`.
   Narrow only complete subsequence membership with unchanged matching rules
   and `catalog_gen`; never use truncated top-k as narrowing input.
   For large non-path batches, union basename bitmap candidates, channel hits
   and matching parent entries, then score using preallocated worker scratch.
   `fuzzy` scores subsequence matches and edit-distance matches separately.
3. `rank` orders lexical results with personalization; the daemon sends the
   `lexical` phase response immediately, or a terminal `final` in lexical-only mode.
4. **Optional semantic:** embed the query and search the active `emb_gen`.
   Float cosine search is the reference; binary/int8 search must pass recall and
   latency evaluation before becoming the default. `rank` fuses with RRF; exact
   basename matches keep priority. The daemon sends the `final` phase response,
   or discards obsolete output if a newer request from that client arrived.
   Both phases use the same snapshot pair. Failures/deadlines finish with lexical
   fallback and a status/reason; active requests must not wait indefinitely.
5. Release pinned snapshots after terminal completion/cancellation. No query
   waits on SQLite or background index updates. Results carry `display` and
   exact `path` (valid UTF-8) or `path_b64` (other bytes). File ids are strings.
6. Clients suppress obsolete responses and preserve selection by file id. Before
   launch, resolve ids against the current catalog; handle stale entries. Open
   recording carries a unique event id plus file/search ids and is asynchronous.
   Launch history tracks accepted requests, not external application success.

Measure engine and client round-trip latency separately, including query
embedding. Track p50/p95/p99, first query, indexing load, update lag, and total/peak
RSS. Time both phases from request acceptance, including queue wait. All
performance numbers in `PLAN.md` are targets until benchmarked.

## Threads

M2 uses the main/query thread and one writer thread, including crawl/watch and
history. Each reader workspace for a large engine also owns three prestarted
scoring workers; the daemon's default reader bound is one. They sleep between
queries and are joined when the workspace is reclaimed. The expanded split
below is the semantic target architecture.

| Thread | Work | Blocks on |
|---|---|---|
| main / query | IPC, pinned lexical search, ranking, history enqueue | bounded socket operations |
| semantic | query embedding, vector search, fusion, `final` response | model inference |
| crawler / watcher | scans, coalesced events, reconciliation requests | filesystem / inotify |
| embedder | versioned path embeddings; optional backend | model inference |
| writer | serialized catalog/history writes, `catalog_gen` publication | SQLite; private index construction |

Vectors are preloaded into owned memory, not searched via file-backed mappings.
This does not guarantee no OS paging/swapping under memory pressure.
The lexical/ranking path uses preallocated scratch, with no filesystem reads,
SQL, or global heap allocation. Profile embedder/runtime allocations separately.
History saturation may drop events with a counter; filesystem queue saturation
must schedule reconciliation. Reclaim `catalog_gen`s only after readers release them.
Bound clients, scratch, membership caches, pending inference, and output queues.
Nonblocking socket I/O and client deadlines prevent stalled consumers from
blocking searches or holding old snapshots indefinitely. Model staging and
old snapshots count toward peak RSS.
Protect snapshot acquisition against reclamation with a short lifecycle lock
or validated epoch scheme. A pointer load followed by reference increment alone
is unsafe. Reclaim retired blocks in the background outside the lifecycle lock.

## M3 desktop flow

The daemon owns an independent XDG desktop catalog and refresh worker. Localized
names, independent generic names and keywords use the lexical engine; typed desktop
results merge with file results under short leases. catalog_gen remains the
file-catalog version. Application IDs are session-scoped and retired on changes.
The file engine carries immutable directory flags for UI icons.

GTK's single-instance popup communicates only through asynchronous IPC exchanges.
Its pure model suppresses obsolete responses and retains deliberate selection.
Actions resolve current identities and run native GIO launch or file open/reveal
in a worker, then enqueue accepted history. SQLite schema v3 stores desktop opens
separately from file opens through the existing writer queue. See ADR 0015 and
[desktop setup](desktop-setup.md).
