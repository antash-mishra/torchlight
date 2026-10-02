# Architecture

## Current implementation

M1 runs locally: `index` wires config -> crawl -> store (configured roots,
allowlists, root deduplication, one transaction per run); `query` loads the
catalog into a sealed lexical engine (prefix, subsequence, trigram and typo
channels over basenames, interned parent directories via `dirtree`), creates
bounded scratch, and searches. The CLI is not yet a socket client and rebuilds
the engine per query. The daemon, threads, snapshot publication, semantics,
history and UI described below remain the target architecture.

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
bin/, ui/  →  ipc/, fs/, storage/, index/  →  core/
```

Modules only depend downward. `core/` depends on nothing in Torchlight.

## Indexing flow

1. `crawl` walks the roots and emits `(path, is_dir, mtime, size)`.
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
   `catalog_gen`/`emb_gen` pair. **Lexical:** run `prefix`, `subseq`, and `trigram`,
   plus eligible one-edit `typo` lookup unless an exact full-basename match
   makes it unnecessary. One/two-character queries use `prefix` and `subseq`.
   Narrow only complete subsequence membership with unchanged matching rules
   and `catalog_gen`; never use truncated top-k as narrowing input.
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
