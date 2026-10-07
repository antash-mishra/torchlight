# Torchlight — Plan

A Spotlight-style launcher for Linux: hit a hotkey, type, and get ranked
files/folders. Written in C. M1/M2 index **filenames and paths**; M3 adds
installed applications and system-settings search. Semantic search uses names,
path context and application metadata; document content extraction is a possible
later extension.

## Implementation progress

M1, M2, M3 and M3 Part 2 are implemented. M4's functional opt-in semantic
path is implemented, with acceptance gates still open. The next priority is
**M6: worker separation → Frizbee SIMD search → incremental indexing**, grouped
as one milestone. M5 personalization follows M6 and the remaining M4 acceptance
work. Existing milestone identifiers are retained; M6 executes next despite
being appended after M5. See [ADR 0026](docs/adr/0026-search-workers-simd-and-incremental-indexing.md).
M6 is implemented in all three steps: a dedicated search thread with per-client
request queues, supersession and cooperative cancellation, and a writer split
into indexing and persistence threads ([ADR 0028](docs/adr/0028-m6-search-thread-and-persistence-owner.md));
Frizbee SIMD scoring with cross-query word caches, bringing 500k typing p95
under 5 ms with unchanged recall ([ADR 0029](docs/adr/0029-m6-frizbee-scoring-and-candidate-volume.md));
and incremental indexing with scoped rescans and base-plus-delta snapshots of
both the lexical engine and the semantic vectors, publishing a 100-file update
in about 110 ms at 500k ([ADR 0030](docs/adr/0030-m6-incremental-indexing.md)).
M3 adds installed application/settings search and the GTK4 popup described in the
[GUI design](docs/m3-gui-design.md).
See [desktop setup](docs/desktop-setup.md) and the
[M3 verification report](docs/m3-completion.md) for usage, checks and measured
limits. The [interactive preview](docs/m3-gui-preview.html) remains the original
file-result design reference.

M4's functional English-first path is implemented: native Potion 256d,
float/int8 retrieval, versioned background cache, exact-priority RRF and coherent
two-phase service. It is opt-in with `torchlightd --model PATH.tlm`.
[M4 implementation](docs/m4-implementation.md),
[model evaluation](docs/m4-model-evaluation.md) and
[ADR 0023](docs/adr/0023-m4-native-potion-and-two-phase-search.md) record the
provisional selection and remaining 500k latency/broader relevance acceptance.

M3 Part 2 strengthens the existing name search before adding embeddings:
field-aware ranking, unfinished-word typo retrieval, better fuzzy scoring and
real launcher relevance tests. The application-name weighting and stable tie
ordering in [ADR 0018](docs/adr/0018-application-name-ranking.md) are already
implemented. Part 2 now adds explicit generic/keyword fields, indexed prefix
edits and bounded optimal fuzzy alignment, with held-out and BM25 comparisons.
See [Part 2 verification](docs/m3-part2-completion.md),
[ADR 0019](docs/adr/0019-search-quality-before-semantic-personalization.md) and
the [search quality review](docs/search-quality.md) for scope and comparisons.

The [M3 review fixes](docs/adr/0017-m3-snapshot-cancellation-and-acceptance.md)
cover atomic desktop replacement, canceled actions, keyboard Retry, named
accessible input and small-screen scale-2 placement. Automated GUI checks now
include AT-SPI and native paint timing during a 500k rebuild. Physical fractional
and multiple-monitor testing, human screen-reader use and Wayland remain
separate platform validation. See [milestone status](docs/milestone-status.md)
for a plain-language account of completed and future work.

The user accepted roughly 6 ms p95 lexical latency at 500k paths for starting M3.
After the search-quality work, the recorded native lexical run has 8.551 ms
typing p95 and 9.622 ms whole-query p95 at 500k paths, excluding IPC/UI.
See [the recorded run](tests/bench/results/2026-10-05-m4-native-lexical.txt).
M6 replaced routine full-engine rebuilds with delta publication (full rebuilds
remain for startup, recovery and compaction) and met the 5 ms typing target;
see the [M6 plan](docs/m6-plan.md) measurements. Historical pre-M3
measurements are in the [readiness report](docs/m3-readiness.md).

The readiness fixes (ADR 0013) remove trigram sorting's indirect heap allocation
and enforce the documented 3–32-symbol typo-query range. Complete resident bitmap
filtering, bounded evidence caches and prestarted scoring workers reduce query
latency without truncating candidates or changing ranking.

The two findings from the [M2 implementation review](docs/m2-review.md) are fixed
(ADR 0012): schema v2 persists filesystem incarnations so replacement files and
directories receive fresh ids; inotify instance failure permits periodic and
explicit scans with degraded watch status. Sanitizer regressions cover rollback,
history, restart replacement, migration and watcher exhaustion/recovery.

M2's functional scope is implemented (ADRs 0010/0011): `torchlightd` serves saved
resident snapshots over bounded Unix-socket IPC; the CLI is a socket client
unless `--db` selects local mode. A background writer coalesces inotify changes,
reconciles roots, prepares/commits/publishes immutable `catalog_gen`s, recovers
publication failures, and writes optional search/open history asynchronously.
Status, byte-safe resolve, rename identity, watch fallback, singleton locking and
clean/crash restart are covered by integration and injected-failure tests.
Daemon benchmarks also record full-rebuild update lag and peak RSS.

M1's features are implemented. The first increment implemented the Makefile
and checks, XDG data paths, physical crawler, SQLite schema v1 and atomic root
refreshes, approved utf8proc normalization, prefix/initials/subsequence matching,
greedy fuzzy scoring, and local index/query CLI. The synthetic warm-engine
benchmark covers 50k/500k paths. See `docs/evaluation.md` for measured limits.

Review fixes (ADR 0007) keep unreadable scopes and unvisited nested roots during
pruning instead of failing or deleting them, and confine parent subsequence
matches to one directory name.

The M1 feature set is implemented (ADR 0008): trigram overlap retrieval,
deletion-neighbourhood typo lookup with bounded edit scoring, parent matching
per directory name below the indexed roots, complete subsequence membership
narrowing, a configuration file with roots and hidden allowlists, root
deduplication and configuration sync, and a benchmark with labeled tuning and
held-out queries over synthetic and real path corpora. Memory at 500k paths fell
by about two thirds. Historical and current benchmark conditions are recorded
in `docs/evaluation.md`; the accepted M3 latency threshold is described above.

Review fixes (ADR 0009) preserve registered roots when an unavailable configured
spelling cannot establish their canonical identity, and roll back the entire
refresh on a storage callback failure. Regression tests cover root aliases,
ancestor pruning, recovery and injected SQLite write failures.

## Goals and acceptance criteria

- Search ~500k paths from a resident daemon. Target warm lexical-phase p95
  below 5ms and final (hybrid) phase p95 below 10ms on a documented reference
  machine.
  These are targets to measure, not established performance numbers. Roughly
  6ms lexical p95 was accepted for starting M3; M6 now targets the lexical
  optimization, while final hybrid latency remains an open M4 acceptance gate.
- Report engine latency and client round-trip latency separately, including
  query embedding in hybrid measurements. Also measure first query, startup,
  crawl time, update lag, peak memory, and latency during indexing.
  Time both phases from request acceptance, including queue wait; final-phase
  timing does not restart after the lexical response.
- Match exact names, prefixes, character subsequences/abbreviations, and
  bounded typos. Preserve strong exact basename matches when adding semantics.
- M3 Part 2 must retrieve unfinished misspelled words, distinguish names from
  keywords and folder context, and keep strong application-name matches visible
  among competing files. Evaluate realistic mixed-catalog queries and preserve
  exact file/path priority and consistent ranking across result limits.
- From M3, search installed GUI applications and system-settings panels alongside
  files and folders. `display` or `resolution` should return Display settings;
  selecting it and pressing Enter should open the display configuration panel.
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

SQLite is the durable catalog. Queries use resident paths, lexical indexes and
vectors; resident usage summaries remain planned for M5. Load saved entries and
serve them before background reconciliation finishes; startup does not require
a synchronous full-home crawl.

M6 separates the daemon IPC loop from a dedicated search worker, indexing work,
and history/persistence work. The GTK main thread retains asynchronous IPC and
the existing launch worker. Search pins immutable file/application snapshots;
indexing prepares replacements privately, and persistence serializes catalog and
history commits without doing filesystem scans or index construction. Frizbee is
the selected SIMD fuzzy matcher. Resident indexes remain the baseline; a mapped
binary index is not a prerequisite for these changes.

## Lexical retrieval

M3 Part 2 compares BM25 as a baseline for field-aware name/context ranking; it
also remains an option if document content is added later. Filename search needs
explicit prefix, subsequence and typo handling; BM25 alone does not supply these
behaviors, although term rarity can help rank short strings.

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
   Resident basename-mask bitmaps intersect required symbols in word-sized
   batches, then union channel hits and matching parent descendants. This
   remains complete even when common letters select most entries (ADR 0013).
4. **`typo`:** a one-edit deletion-neighbourhood index (SymSpell style) for
   complete basename/stem/token matches, run for query tokens of 3–32 symbols.
   An exact full-basename match may skip this channel; a large
   number of unrelated prefix/subsequence hits may not. Verify retrieved terms
   with edit distance, since shared deletion keys are only candidate signals.
   Lookup includes postings expansion; its cost is not independent of corpus
   size. Measure index memory, update cost, and recall. A bit-parallel scan is
   the benchmark alternative, with any performance gate evaluated for recall.

One/two-character queries use `prefix` and `subseq`.

**M3 Part 2 extension:** retain these channels and add indexed prefix edit
matching for unfinished typo queries, including adjacent swaps. The implemented
sorted token ranges form an implicit trie with bounded edit rows, reusing the existing dictionary.
Use explicit basename/application-name, keyword/generic-name and parent-context
fields so scoring can distinguish complete tokens, partial tokens, typo counts,
word coverage and proximity. Compare optimal fuzzy character alignment with the
greedy comparison scorer; pruning bounds and cached scores include token
completeness. This extension is implemented in ADR 0020. Default query behavior remains independent of result capacity.

Split multiword queries into tokens: each must match a basename or parent-path
token, possibly with different lexical channels. Combine token scores with
basename priority; preserve a separate exact raw-path lookup. Empty queries return
a bounded recent/shortcut list, or indexed root entries when history is disabled.
The GTK popup shows `Type to search` without results for empty or whitespace-only
input; it sends queries only after typing. Specify deterministic tie-breaking so
results do not reshuffle between phases.

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

- After M3 Part 2, compare `minishlab/potion-retrieval-32M` and
  `ibm-granite/granite-embedding-small-english-r2`, retaining
  `BAAI/bge-small-en-v1.5` as a comparison baseline. Include
  `google/embeddinggemma-300m` when evaluating multilingual retrieval.
  Use labeled path/application queries before choosing a default; model-card
  benchmarks do not establish launcher relevance. See
  [the shortlist and sources](docs/search-quality.md#updated-semantic-shortlist-for-evaluation).
- Keep `tl_embedder` swappable. Validate C tokenization and inference against
  reference embeddings. For Model2Vec, evaluate native token lookup/pooling
  and supported ONNX export; do not assume generic `optimum` export works.
- Published Model2Vec scores describe general retrieval benchmarks, not quality
  on abbreviated filenames or indexing time for `$HOME`.
  [Model card](https://huggingface.co/minishlab/potion-retrieval-32M)
- Embed basename, extension, relevant parent context and application metadata.
  Measure whether common directory prefixes add noise and whether the model
  recognizes abbreviations. Desktop embeddings must stay tied to the selected
  entry's metadata/revision across refresh and both response phases.
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
- In M4, fuse lists with RRF as the first baseline and compare a tuned score
  combination on held-out queries; select from measured quality. Give exact
  basename/path matches explicit priority and retain lexical-only behavior when
  the model is unavailable or an entry has not been embedded. Start RRF at k = 60
  and tune it on labeled queries. M5 adds bounded personalization boosts afterward.
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
  -- Schema v2: canonical device/inode + birth timestamp (ctime fallback).
  identity    BLOB CHECK (identity IS NULL OR
                         (typeof(identity) = 'blob' AND length(identity) = 29)),
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

-- Schema v3: desktop launches are keyed by logical desktop id, not a file row.
CREATE TABLE desktop_opens (
  event_id TEXT NOT NULL PRIMARY KEY,
  desktop_id TEXT NOT NULL,
  search_id TEXT REFERENCES searches(id) ON DELETE SET NULL,
  ts INTEGER NOT NULL
);
CREATE INDEX desktop_opens_ts ON desktop_opens(ts);

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
the schema version (currently 3), `catalog_gen`, and the active `emb_gen` configuration. The
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

## Implementation status

M1, M2 and M3 are implemented. M3 adds the resident XDG desktop catalog, mixed
application/settings/file/folder results, asynchronous GTK4 popup and native
actions, installable desktop entry and systemd user unit. See
[desktop setup](docs/desktop-setup.md), [M3 verification](docs/m3-completion.md)
and [ADR 0015](docs/adr/0015-m3-desktop-catalog-and-launcher.md).
M3 Part 2 search quality is implemented; see
[its verification and measured tradeoffs](docs/m3-part2-completion.md). M4 implements opt-in native semantics, background cache and two-phase hybrid
queries; large-catalog latency and broader relevance acceptance remain open.
M6 worker separation, Frizbee SIMD search and incremental indexing are
implemented; M5 personal recommendations follow the remaining M4 acceptance work.
Cinnamon X11 is the verified target; wider desktop/theme/scaling acceptance is
tracked explicitly in the verification report.

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
3. **M3: Usable desktop launcher with application and settings search.**
   After M1/M2, add a catalog of installed applications and settings panels
   discovered from standard user/system XDG `.desktop` entries. Search their
   localized names, generic names and keywords; return application/settings
   results alongside file/folder results through daemon IPC. Respect desktop
   visibility rules, user overrides and application install/remove changes.
   Build the GTK4 popup with keyboard flow, desktop hotkey, application icons,
   application/settings launch actions, file open/reveal actions, and systemd
   user service. Enter activates the selected desktop entry through the desktop
   application-launch API. Verify focus, stale-response handling, and typing
   responsiveness in the target desktop session.
   Acceptance examples: `display`, `screen` or `resolution` finds Display
   settings and opens its panel (`cinnamon-settings display` on Cinnamon);
   `sound` finds Sound settings, `keyboard` finds Keyboard settings, and an
   installed application's name finds and launches that application. Validate
   hidden/incompatible entries, duplicate desktop ids, and removed applications.
   Follow [the GUI specification](docs/m3-gui-design.md) and
   [interactive preview](docs/m3-gui-preview.html), recorded in ADR 0014.
4. **M3 Part 2: Search quality and relevance.** Improve the implemented
   application/settings/file/folder search before introducing semantic results.
   Build a labeled mixed-catalog query set covering strong app-name fragments
   among noisy files, unfinished typos, abbreviations, duplicate names,
   folder-plus-filename queries, extensions, Unicode/raw bytes and no-match
   queries. Label multiple reasonable results for ambiguous queries and separate
   tuning from held-out targets.
   Keep name, keyword/generic-name and folder evidence as explicit fields; rank
   by exactness, token completeness, typo count, coverage, proximity and field
   importance. Add indexed prefix typo retrieval and compare an optimal fuzzy
   scorer against the greedy baseline. Benchmark field-aware token scoring,
   with BM25 as a comparison baseline rather than assuming it handles prefixes
   or typos by itself.
   Acceptance includes `chrome` finding Google Chrome ahead of unrelated file
   prefixes in the mixed fixture; `proej` finding `projectNotes.md` in the
   one-file fixture; and existing exact-name/path, Unicode/raw-byte, warm/cold
   query and top-k consistency regressions continuing to pass. Report candidate
   recall, first useful result, top-ten success and graded nDCG@10 per query
   class. Require measured held-out relevance improvement over the current
   baseline; measure warm typing, query latency and steady/peak RSS at 50k/500k.
   Preserve allocation-free lexical queries and bounded work. The original
   5 ms target and full-rebuild optimization remain separately tracked work.
   Implemented in [ADR 0020](docs/adr/0020-m3-part2-search-quality.md), including
   Chrome and one-file `proej` regressions, explicit metadata fields, optimal
   alignment and held-out/BM25 comparisons. See
   [measured acceptance](docs/m3-part2-completion.md); the small labeled fixture
   and latency tradeoff are explicit limits.
5. **M4: Hybrid semantic search.** After M3 Part 2, compare small local
   embedding models on the labeled filename/path and application-metadata queries.
   Create embeddings in the background; embed each query and retrieve similar
   vectors with float cosine search as the correctness reference. Evaluate
   quantization and approximate retrieval only when latency/RSS measurements
   justify them. Verify C tokenizer/inference parity, reference/quantized recall,
   background updates and model replacement before choosing a backend.
   Combine the improved lexical list with semantic results; start with RRF and
   compare a tuned score combination on held-out queries. Preserve explicit
   exact-name/path priority. Integrate two-phase execution with coherent
   file/desktop metadata and embedding snapshots, bounded queues, cancellation,
   deadline/error fallback and selection stability. Name results appear
   immediately; semantic work never delays the first response. File contents
   remain a possible later extension.
   Functional opt-in implementation in [ADR 0023](docs/adr/0023-m4-native-potion-and-two-phase-search.md):
   native Potion, float/int8 retrieval, persisted background caching and coherent
   two-phase RRF. See [implementation and acceptance gates](docs/m4-implementation.md)
   and [measured model/daemon evaluation](docs/m4-model-evaluation.md); 500k latency
   and broader model/relevance acceptance remain open.
   M6 is implemented; these remaining M4 acceptance gates were re-measured at
   its end and remain open (see the [M6 plan](docs/m6-plan.md)).
6. **M5: Personal recommendations and ranking.** After M6 and remaining M4
   acceptance work, use optional resident frecency and query-to-open summaries
   for both files and applications.
   Apply bounded boosts for frequently/recently opened and previously selected
   results. Respect disabled history, clearing and retention in persisted and
   resident state. Compare hybrid ranking with/without personalization on
   held-out usage scenarios; improve personally useful results without burying
   exact matches or strong name evidence. Existing history recording is
   implemented; recommendation scoring is future work.
7. **M6: Search responsiveness and indexing performance.** Implemented
   (2026-10-07) in this order, as three steps within one milestone:

   - **Worker separation.** Keep UI rendering/input and daemon IPC responsive
     while a dedicated search worker retrieves and ranks file/application
     results. Retain only the newest pending query per client, cancel obsolete
     work between batches, and reject stale completions. Separate indexing from
     history/persistence; serialize catalog/history commits through the database
     owner, with scans and index builds outside write transactions. Preserve
     immutable snapshot leases, semantic deadlines, launch actions and shutdown.
   - **Frizbee SIMD search.** Integrate Frizbee through its C ABI as the production
     fuzzy matcher; pin the dependency and document its build. The user selected
     this library; no library-selection or comparative matcher evaluation phase
     is required. Keep prefix/subsequence/trigram/typo retrieval, explicit field
     weights, exact-name/path priority, Unicode normalization and raw-byte paths.
     Adapt scratch and score bounds; reuse one bounded scoring pool where needed.
   - **Incremental indexing.** Apply coalesced filesystem changes to affected
     entries and directories without routinely scanning every root or rebuilding
     the full engine. Share unchanged immutable blocks, batch publication and
     compact in the background. Preserve commit-before-publication, rename and
     replacement identity, semantic invalidation, overflow reconciliation and
     restart recovery; retain full rebuilds for recovery and compaction.

   Acceptance: sanitizer/lint and existing search-quality regressions pass;
   rapid typing serves the latest request without an obsolete-query backlog;
   query work remains allocation-free and does not wait for SQL or filesystem
   I/O. Record engine/IPC p50/p95/p99, indexing-load latency, update lag, history
   queue counters and steady/peak RSS at 50k/500k. Target warm lexical p95 below
   5 ms and demonstrate lower small-update lag and peak RSS against the recorded
   full-rebuild baseline. These are implementation checks, not a library choice.
   M6 does not add personalization, document-content search or a required mmap
   file format. See [ADR 0026](docs/adr/0026-search-workers-simd-and-incremental-indexing.md).
   The [M6 working plan](docs/m6-plan.md) refines these steps from a code
   survey and a 500k profile: scoring is about 46% of query time, so step 2
   pairs Frizbee with candidate-volume reduction; step 3 splits into scoped
   reconcile, a base-plus-delta segmented engine, and incremental semantic
   snapshots. It records the baseline numbers each step must beat.

## Evaluation

The labeled query set, metrics, regression scenarios and benchmark reporting
rules are in [`docs/evaluation.md`](docs/evaluation.md).
The [search quality review](docs/search-quality.md) records observed relevance
gaps and the implemented application-name ranking fix. M3 Part 2 and M4 have
recorded quality/performance results; M4 acceptance and M5 personalization retain
their evaluation work. M6 validates the selected Frizbee integration and worker/
indexing changes against existing quality contracts and measured latency/memory.
It does not reopen matcher selection. Feature completion does not establish best
search quality.

## Build

- Build with C17 and a plain Makefile. Current dependencies are SQLite,
  utf8proc, GIO/GIO-Unix, GTK4 4.14+ and X11. ONNX Runtime is a possible M4
  dependency if the selected backend needs it; ncurses remains optional.
  IPC uses the bounded core JSON codec rather than adding cJSON.
  Frizbee is selected and authorized for M6 through its C ABI; pin its Rust
  library/build tooling during integration. Discuss other additions before
  implementation, following `AGENTS.md`.
- Run sanitizers, unit/integration checks, lint, and relevant benchmarks as
  milestones introduce code. Planning changes require document consistency.

Repository layout and module rules are in `AGENTS.md` and `CLAUDE.md`.
