# Torchlight — Plan

A Spotlight-style launcher for Linux: hit a hotkey, type, and get ranked
files/folders. Written in C. The first version indexes **filenames and paths**.
Semantic search uses only information present in those paths; document content
extraction is a possible later extension.

## Implementation progress

M1 is in progress. The first increment implements the Makefile/checks, XDG data
paths, physical crawler, SQLite schema v1 and atomic root refreshes, approved
utf8proc normalization, prefix/initials/subsequence matching, greedy fuzzy
scoring, and local index/query CLI. The synthetic warm-engine benchmark covers
50k/500k paths. See `docs/evaluation.md` for measured limits.

Review fixes (ADR 0007) keep unreadable scopes and unvisited nested roots during
pruning instead of failing or deleting them, and confine parent subsequence
matches to one directory name.

Remaining M1 work: trigram overlap retrieval, deletion-neighbourhood typo lookup
and edit scoring; strict parent-token matching and ranking evaluation; complete
subsequence membership narrowing; config/hidden allowlists/root deduplication;
and latency/memory improvements with representative labeled/held-out corpora.
M1 is not complete and its latency target is not met by the first baseline.

## Goals and acceptance criteria

- Search ~500k paths from a resident daemon. Target warm lexical-phase p95
  below 5ms and final (hybrid) phase p95 below 10ms on a documented reference
  machine.
  These are targets to measure, not established performance numbers.
- Report engine latency and client round-trip latency separately, including
  query embedding in hybrid measurements. Also measure first query, startup,
  crawl time, update lag, peak memory, and latency during indexing.
  Time both phases from request acceptance, including queue wait; final-phase
  timing does not restart after the lexical response.
- Match exact names, prefixes, character subsequences/abbreviations, and
  bounded typos. Preserve strong exact basename matches when adding semantics.
- Support semantic matches when names or parent folders carry meaning.
  `tax receipts` → `ITR_2024_ack.pdf` is an evaluation case, not a guarantee.
- Return lexical results while embeddings are unavailable or incomplete;
  expose indexing status to clients.
- Persist the catalog and optional search/open history in SQLite. Support
  disabling history, clearing it, and configuring retention.

## Architecture

```text
CLI / GTK4 popup
       │ Unix socket
       ▼
torchlightd (C)
  ├─ lexical: prefix, trigram, subseq, typo channels + fuzzy/edit scorer
  ├─ optional embedder + vector search
  ├─ ranker + resident usage statistics
  ├─ crawler + inotify watcher
  └─ background writer + SQLite (WAL)
```

SQLite is the durable catalog. Queries use resident paths, lexical indexes,
vectors, and usage summaries. Load saved entries and serve them before background
reconciliation finishes; startup does not require a synchronous full-home crawl.

## Lexical retrieval

BM25 remains an option if document content is added later. Filename search needs
explicit prefix, subsequence, and typo handling; BM25 alone does not supply these
behaviors, although term rarity can still help rank short strings.

The `lexical` module combines four channel modules before scoring:

1. **`prefix`:** exact basename/path, basename/token prefix, and token-initials
   lookup (`pn` → `projectNotes`).
2. **`trigram`:** relaxed overlap retrieval for substrings and typos, rather
   than requiring every query trigram to occur.
3. **`subseq`:** a per-path 64-bit character mask scan, then an ordered
   subsequence check for abbreviations. Always runs: `prjnts` has no contiguous
   trigrams in common with `projectNotes.md`. Incremental narrowing: when the
   normalized query extends the previous one, recheck its complete subsequence
   membership, before ranking/truncation, and only for the same `catalog_gen`
   and matching configuration. Otherwise rescan. A bounded cache that cannot
   hold complete membership disables narrowing rather than losing matches.
   Character posting lists are a benchmark alternative only, because common
   letters make their intersections close to a full scan.
4. **`typo`:** a one-edit deletion-neighbourhood index (SymSpell style) for
   complete basename/stem/token matches, run for query tokens of at least three
   characters. An exact full-basename match may skip this channel; a large
   number of unrelated prefix/subsequence hits may not. Verify retrieved terms
   with edit distance, since shared deletion keys are only candidate signals.
   Lookup includes postings expansion; its cost is not independent of corpus
   size. Measure index memory, update cost, and recall. A bit-parallel scan is
   the benchmark alternative, with any performance gate evaluated for recall.

One/two-character queries use `prefix` and `subseq`.

Split multiword queries into tokens: each must match a basename or parent-path
token, possibly with different lexical channels. Combine token scores with
basename priority; preserve a separate exact raw-path lookup. Empty queries return
a bounded recent/shortcut list, or indexed root entries when history is disabled.
Specify deterministic tie-breaking so results do not reshuffle between phases.

Deduplicate candidates and favor basename matches over parent-path matches.
Use fzf/fzy-style boundary, camelCase, and consecutive-character bonuses with gap
penalties for subsequences. Use a separate bounded edit-distance scorer for
insertions, deletions, and substitutions; subsequence scoring alone is not typo
correction. Start with one edit for queries of at least three characters, then
validate the budget against labeled queries.

Benchmark candidate budgets and fallback scans for recall and latency. Rank cheap
candidate signals before imposing limits; do not truncate by file id. Measure
whether known targets survive candidate selection.

Linux paths are bytes and may not be valid UTF-8. Store and act on exact bytes.
Use canonical normalization (NFC) and Unicode case-folding for valid UTF-8 runs;
keep invalid bytes as distinct opaque symbols, never replacement characters in
matching keys. Use the same versioned normalization for paths and queries.
Evaluate a Unicode C library such as `utf8proc` before implementing this behavior;
its dependency addition requires the discussion in `AGENTS.md`.
[utf8proc documentation](https://juliastrings.github.io/utf8proc/doc/)

Derive a `display` string (invalid bytes → U+FFFD) that is never used to open
files. Keep character-boundary metadata for scoring. Split `/ _ - . space` and
camelCase. Trigram keys are collision-checked hashes or byte tuples. Use
codepoints/opaque symbols for subsequence and edit checks, and normalized byte
trigrams for the trigram channel. The 64-bit mask is a conservative filter:
hash all matching symbols into bits or bypass filtering for unsupported symbols;
collisions may add candidates but must never exclude valid matches.

SQLite FTS5 trigram is an experimental baseline, not the default resident engine.
It does not supply subsequence matching or full-text matches below three
characters. [SQLite FTS5 documentation](https://www.sqlite.org/fts5.html)

## Semantic retrieval and ranking

- Compare `minishlab/potion-retrieval-32M`,
  `ibm-granite/granite-embedding-30m-english`, and `BAAI/bge-small-en-v1.5`
  on real path/query pairs before choosing a default.
- Keep `tl_embedder` swappable. Validate C tokenization and inference against
  reference embeddings. For Model2Vec, evaluate native token lookup/pooling
  and supported ONNX export; do not assume generic `optimum` export works.
- Published Model2Vec scores describe general retrieval benchmarks, not quality
  on abbreviated filenames or indexing time for `$HOME`.
  [Model card](https://huggingface.co/minishlab/potion-retrieval-32M)
- Embed basename, extension, and relevant parent context. Measure whether common
  directory prefixes add noise and whether the model recognizes abbreviations.
- **Two-phase responses:** send the lexical results immediately, then a fused
  `final` response for the same request id once query embedding and vector
  search finish. Cancel stale semantic work. Typing latency never waits on
  inference. Assign the search id and enqueue history before the first response;
  both phases share that id and one pinned `catalog_gen`/`emb_gen` pair.
  Lexical-only mode returns one `final`. Missing models, inference failures,
  and deadline expiry finish with lexical results and a reason/status field.
  Every active, uncancelled request receives a terminal response; never leave
  a CLI waiting indefinitely for a semantic result.
  Discard queued obsolete work and ignore already-running obsolete inference;
  backend preemption is not assumed. Cancellation is per client/request, so one
  client's typing cannot cancel another client's search.
- Build normalized float cosine search as a correctness/recall reference first.
  Evaluate sign-bit Hamming scan plus int8 rescoring as an optimization. Measure
  recall against that reference and tune the shortlist; 200 is an experiment.
  Persist int8 scales and normalization metadata needed for comparable scores.
- Measure scan time, total latency, and recall at 50k and 500k paths. Introduce
  ANN only if measured latency/recall requires it; no 1–2ms scan is assumed.
- Fuse lists with RRF, then apply bounded personalization boosts. Give exact
  basename matches explicit priority and retain lexical-only behavior when the
  model is unavailable or an entry has not been embedded. Start RRF at k = 60
  and tune it on labeled queries.
- Model revision, preprocessing/tokenizer version, dimension, and quantization
  format define an embedding generation (`emb_gen`). Build replacements in the
  background, validate them, then activate them together. Never mix `emb_gen`s.

### Memory budget

For 500k paths, vector payload alone costs:

| Dimensions | Binary | Int8 | Combined |
|---|---:|---:|---:|
| 256 | 16MB | 128MB | 144MB |
| 384 | 24MB | 192MB | 216MB |
| 512 | 32MB | 256MB | 288MB |

These are decimal MB and exclude scales, ids, paths, posting lists, model
weights/runtime, SQLite caches, and temporary `emb_gen` staging.

**Decision:** preload vectors into owned memory rather than query a file-backed
mapping. This avoids explicit file reads but does not guarantee no page faults
or swapping under memory pressure. Warm buffers and measure page faults/RSS.
The default budget for binary + rescoring vector payload is
150MB at 500k paths (configurable). Meet it by reducing dimensions first (PCA
for Model2Vec, truncation for Matryoshka-trained models), measured against the
original full-dimensional float reference as well as reduced float vectors.
Fit any projection on calibration data, persist its checksum in `emb_gen`, and
apply the identical transform and normalization to queries and paths. If quality
needs more dimensions, evaluate int4 rescoring, a larger configured budget, or
binary-only retrieval on its own merits. Enlarging a shortlist without a more
accurate scorer cannot recover information lost to binary quantization.
Measure total steady-state and peak RSS, including old/staged `emb_gen`s,
lexical/typo indexes, and per-client caches. Limit outstanding work and cache
memory; delay model replacement if staging exceeds the configured total budget.
The float reference need not stay loaded in production.

## Index consistency and recovery

- One background writer serializes catalog changes. Prepare and validate its
  private index delta first, commit the catalog batch to SQLite, then publish its
  `catalog_gen`. Failed commits keep the previous `catalog_gen` active and
  trigger retry/reconciliation.
  If publication cannot complete after commit, reload/rebuild from SQLite before
  accepting later updates; keep serving the previous snapshot with degraded status.
- Build changes privately. Each query pins an immutable `catalog_gen` of paths,
  postings, vector mappings, and usage summaries. Publish via a short pointer
  swap; reclaim old blocks after readers release them. Batch updates and share
  unchanged blocks to control peak memory.
  Protect snapshot acquisition against concurrent reclamation with a short
  lifecycle lock (or a validated epoch scheme); loading a pointer and then
  incrementing its reference count is not sufficient. Free retired blocks on
  the background thread, outside that lock.
- Use monotonically allocated file ids (`AUTOINCREMENT`). Remove deleted entries
  from postings/vectors and reject stale opens. Paired renames preserve ids;
  ambiguous moves are reconciled as delete/add. Directory renames update all
  descendants and invalidate their path embeddings.
- Install watches during crawling and reconcile affected directories afterward.
  Coalesce events, pair move cookies where possible, and handle event overflow,
  watch exhaustion, and unavailable roots.
- Overflow triggers reconciliation. Watch exhaustion uses periodic rescans for
  unwatched subtrees. Reconcile at restart and periodically. Retire missing
  entries only after a successful scan of their scope; an unreadable/offline
  subtree must not be mistaken for deletion.
- Use preallocated query scratch and bounded background queues. A full history
  queue may drop history with a diagnostic counter; lost filesystem updates
  must schedule reconciliation.
- Bound client count, candidate scratch, cached membership, in-flight semantic
  work, and pinned snapshots. Use nonblocking socket I/O with bounded output
  queues; disconnect a slow client rather than block queries or retain snapshots
  indefinitely. Prioritize current interactive semantic work over path embedding.
- The lexical/ranking path performs no filesystem I/O, SQLite calls, or global
  heap allocation. Profile embedder allocations separately before claiming an
  allocation-free hybrid query. Vectors are preloaded, not queried from files.
- Code and docs always say `catalog_gen` or `emb_gen`, never a bare "generation".

## Persistence schema (initial design)

```sql
PRAGMA foreign_keys = ON;

CREATE TABLE files (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  -- Paths are raw bytes. The CHECKs stop a TEXT bind from creating a second
  -- row: SQLite treats TEXT and BLOB with identical bytes as different values.
  path        BLOB UNIQUE NOT NULL CHECK (typeof(path) = 'blob'),
  name        BLOB NOT NULL CHECK (typeof(name) = 'blob'),
  ext         BLOB CHECK (ext IS NULL OR typeof(ext) = 'blob'),
  is_dir      INTEGER NOT NULL CHECK (is_dir IN (0, 1)),
  mtime       INTEGER,
  size        INTEGER,
  emb_version TEXT,
  emb_bin     BLOB,
  emb_i8      BLOB,
  emb_scale   REAL
);

CREATE TABLE searches (
  id    TEXT NOT NULL PRIMARY KEY, -- unique daemon session id + query sequence
  query TEXT NOT NULL,
  ts    INTEGER NOT NULL
);

CREATE TABLE opens (
  event_id  TEXT NOT NULL PRIMARY KEY, -- deduplicates retried launch records
  file_id   INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,
  search_id TEXT REFERENCES searches(id) ON DELETE SET NULL,
  ts        INTEGER NOT NULL
);
CREATE INDEX opens_file_ts ON opens(file_id, ts);
CREATE INDEX opens_search ON opens(search_id);

-- Registered successfully scanned roots support empty-query results in M1.
CREATE TABLE roots (
  path BLOB PRIMARY KEY NOT NULL CHECK (typeof(path) = 'blob')
);

CREATE TABLE meta (
  key   TEXT NOT NULL PRIMARY KEY,
  value TEXT
);
```

Use prepared statements, migrations, and batched transactions. Always bind
paths, names and extensions with `sqlite3_bind_blob`. `meta` records
the schema version, `catalog_gen`, and the active `emb_gen` configuration. The
initial schema covers one active `emb_gen`; M4 adds staging storage for model
replacement.
Assign search ids in memory, using a unique session id generated at startup.
History writes preserve search-before-open ordering; opens without retained
search history use a null `search_id`. Path updates invalidate embeddings even
when mtime is unchanged. Catalog writes continue when history is disabled.

## Filesystem, IPC, and UI

- **Crawler:** configurable roots, initially `$HOME`. Hidden directories are
  skipped by default (covering `.git`, `.cache`, `.cargo`, `.npm`, `.mozilla`,
  `.local/share/...`), with a config allowlist to re-include specific ones.
  Hidden files in visible directories are indexed. Visible trees ignored by
  default: `node_modules`, `target`, `build`, `__pycache__`.
  Index symlinks without traversing directory symlinks. Handle permission
  errors and unavailable roots without deleting their saved entries.
  Explicitly configured roots override hidden-directory defaults; allowlisted
  descendants require traversal of their hidden ancestors. Canonicalize/deduplicate
  overlapping roots, and rescan/prune successfully excluded scopes when config
  changes. Keep unreadable scopes distinct from intentional exclusion.
- **Storage/config:** catalog under `$XDG_DATA_HOME/torchlight` (fallback
  `~/.local/share/torchlight`), config under `$XDG_CONFIG_HOME/torchlight` (fallback
  `~/.config/torchlight`). Exclude Torchlight's own state/model directory even
  if its parent is allowlisted. Apply retention/clear operations to persisted
  history and resident personalization summaries; history is not an unbounded log.
- **IPC:** versioned, size-bounded JSON-line requests/responses at
  `$XDG_RUNTIME_DIR/torchlight.sock`, restricted to the current user. Queries
  carry request ids; responses carry request/search ids, `catalog_gen`,
  indexing status, a `phase` (`lexical` or `final`), and bounded results. Each
  result has an id and UTF-8 `display` string, plus an exact unnormalized `path`
  for valid UTF-8 or `path_b64` for other byte paths. Display may be abbreviated
  and is never an action target. Encode file ids as decimal strings to avoid
  JSON clients losing 64-bit precision. Reject NUL-containing decoded paths.
  Escape queries and display strings, including embedded newlines. Cancel queued obsolete queries and suppress stale UI responses.
  A resolve request validates a file id against the current catalog and returns
  its current exact path or a stale-result error before launch. Open-recording
  requests carry file/search ids; recording is asynchronous.
  Request ids are scoped to a connection; reject duplicate active ids, and use
  a unique launch-event id to deduplicate retried history records.
- **UI:** CLI first, then GTK4 popup. Configure the desktop environment's global
  shortcut to invoke/toggle it and check focus on the user's X11/Wayland session.
  Render the lexical phase at once, then update in place with the final phase.
  Keep selection by file id during final-phase reordering; once the user selects
  a row, retain that row until they move selection or change the query. Resolve
  before opening; deleted/moved results must not launch a cached, reused path.
  Arrow keys select, Enter opens, Escape dismisses. Actions use exact path bytes.
  Ctrl+Enter reveals through the file manager's D-Bus interface, falling back to opening the parent.
  Spawn open commands with argv, without shell interpolation. History records
  accepted launch requests, not guaranteed success in external applications.
- **Service:** systemd user unit, status reporting, clean shutdown/restart,
  and a single daemon instance. A TUI is optional after the popup works.

## Milestones

1. **M1: Lexical engine and CLI.** Build tooling, crawler, SQLite catalog,
   `lexical` with `prefix`/`trigram`/`subseq`/`typo` channels, fuzzy/edit
   scoring, and index/query CLI. Start with prefix/subsequence, add trigram,
   then typo retrieval in small benchmarked increments. Include non-UTF-8 paths
   in fixtures. Use a reusable engine benchmark to measure warm queries
   separately from CLI startup/loading.
   Validate names, prefixes, abbreviations, short queries, and typos at 50k/500k.
2. **M2: Resident daemon.** Socket with a terminal lexical-only response and
   protocol fields for later two-phase results, immutable `catalog_gen`s,
   asynchronous writer and history, inotify, reconciliation, and status reporting. Validate
   create/delete/rename, directory moves, concurrent queries/updates, overflow,
   watch exhaustion, unreadable roots, and crash/restart recovery.
3. **M3: Usable desktop launcher.** GTK4 popup, keyboard flow, desktop hotkey,
   open/reveal actions, and systemd user service. Verify focus, stale-response
   handling, and typing responsiveness in the target desktop session.
4. **M4: Semantic search.** Compare models on labeled path queries. Verify C
   embedding parity, float reference, quantized recall, total latency/RSS,
   background embedding, and model replacement. Choose the model/vector
   strategy from measurements, then integrate RRF and two-phase execution with
   shared snapshots, bounded queues, cancellation, and deadline/error fallback.
5. **M5: Personalization.** Resident frecency/query-open summaries, bounded
   boosts, history controls and retention. Compare ranking with/without
   personalization and prevent popular files from burying exact matches.

## Evaluation

The labeled query set, metrics, regression scenarios and benchmark reporting
rules are in [`docs/evaluation.md`](docs/evaluation.md).

## Build

- Build with C17 and a plain Makefile. Planned dependencies: SQLite first,
  cJSON for IPC, GTK4 for the popup, and ONNX Runtime if the selected backend
  needs it. Evaluate utf8proc for the normalization contract; ncurses is
  optional. Discuss additions before implementation, following `AGENTS.md`.
- Run sanitizers, unit/integration checks, lint, and relevant benchmarks as
  milestones introduce code. Planning changes require document consistency.

Repository layout and module rules are in `AGENTS.md` and `CLAUDE.md`.
