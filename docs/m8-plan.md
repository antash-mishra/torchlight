# M8 plan: rank files by place, and let people exclude folders

**Status: implemented (2026-10-12).** Both parts are done; see the
[results](#results-2026-10-12), [ADR 0047](adr/0047-place-ranking-and-excluded-folders.md)
and the [health report](reports/m8-place-ranking-health-report.html): 32
checks in range, 4 to watch, and 2 out of range that read the same on HEAD
(a full check at its 1 s limit on a busy evening, and the M7 Phase 3
rebuild-memory item). This is the working plan for two changes to file
ranking and indexing scope.

## In simple words

When you type `sc`, the applications at the top are right, but the files
below them come from places nobody searches: the Android SDK and the Go
module cache. That is not because Torchlight prefers deep folders. File
ranking knows nothing about where a file lives. Every folder whose name
starts with `sc` gets the same score, shorter names win, and ties are broken
alphabetically by full path. `/home/antash/Android/...` sorts first because
an uppercase `A` sorts before every lowercase letter.

On the live catalog (213,356 entries) 66% of entries are under
`~/Android/Sdk` and 18% under `~/go/pkg`. They win every tie by sheer
numbers. Even typing `scripts` exactly lists two Android NDK `venv/scripts`
folders before `~/workspace/torchlight/scripts`.

This milestone adds two things:

1. **A place penalty** (Part A). Each file gets a small, fixed penalty from
   its path alone: deeper below the indexed folder costs more, and anything
   inside a dependency or toolchain tree (a package cache, vendored code, an
   SDK, a versioned install folder) costs more again. The penalty reorders
   files of similar match strength. It never lifts a weaker kind of match
   above a stronger one.
2. **An `exclude` setting** (Part B). `exclude = ~/Android/Sdk` in the
   configuration file stops Torchlight from indexing that folder at all.

## Baseline (2026-10-12, live catalog, current build)

A simulation with the real engine over the live catalog, using 30 everyday
queries (`sc`, `scripts`, `readme`, `makefile`, `main`, `config`, ...).
"Third-party" means a path under the Android SDK, the Go module cache, the
Vulkan SDKs, a downloaded tool, `nltk_data`, or any `site-packages`,
`node_modules`, `venv`, `third_party` or `vendor` folder. "Own" means a path
under `workspace`, `Documents`, `Desktop`, `AndroidStudioProjects`,
`Pictures`, `Videos` or `Downloads` that is not third-party.

| Measure | Current |
|---|---|
| Third-party results in the top 10, 30 queries | 186 of 300 |
| Own results in the top 10 | 78 of 300 |
| `sc`: own results in the top 10 | 0 |
| `scripts`: rank of `~/workspace/torchlight/scripts` | below 10 |

The simulated penalty (depth beyond three levels, 24 per level, 128 more
inside a dependency tree, capped at 256) gave 88 third-party and 137 own
results for the same queries. Caps from 256 to 600 performed alike, so the
smallest cap was chosen: most of the gain comes from reordering ties.

## Part A: place penalty

### Design

- **Where it lives.** A new `place` module (`src/index/place.c`) holds the
  policy as pure functions over normalized directory names: no allocation,
  no I/O. `lexical` calls it once per interned directory when an engine is
  sealed and stores each entry's 16-bit penalty (2 bytes per entry; first
  built per directory node, see the typing results below). A query reads one
  extra value per scored result.
- **Levels.** A directory's level counts the usable directories on its path:
  the indexed root's own folder is level 1. A file's depth is its parent's
  level minus one, so `~/notes.txt` is at depth 0 and
  `~/workspace/torchlight/scripts` at depth 2. Directories above the indexed
  roots never count, as for parent matching.
- **Dependency trees.** A directory is a dependency marker when its
  normalized name is one of `node_modules`, `bower_components`,
  `site-packages`, `dist-packages`, `vendor`, `third_party`, `third-party`,
  `thirdparty`, `sysroot` or `sdk`; when it is a Go module directory
  (contains `@v` followed by a digit, or is exactly `@v`); or when it is a
  versioned install folder (an optional `v`, one to three digits, then one or
  more `.digits` groups, such as `27.0.12077973` or `1.4.335.0`; four-digit
  years such as `2024.05.01` do not count). Everything below a marker is in
  a dependency tree; the marker folder itself is not, so searching
  `site-packages` still finds the folder first.
- **Penalty.** `24 × max(0, depth − 3)`, plus 128 inside a dependency tree,
  capped at 256. Named constants with static assertions.
- **Scores.** An ordinary result scores `match + length bonus − penalty`,
  at least 1. An exact name scores `exact-name tier − penalty`. Exact raw
  paths and indexed roots (the empty query) are not penalized.
  `lexical_exactness` keeps classifying both exact tiers.
- **Tiers.** The cap (256) stays below the smallest gap between channel
  tiers (initials 4500 to token prefix 5000), as personal frecency does
  (ADR 0033): a deep basename prefix still beats a shallow token prefix, and
  an exact name still beats everything but an exact path. It can override the
  128 completion bonus and the 64 length bonus, which is the point: those are
  weaker signals than place.
- **Exact pruning.** Every query shortcut compares an upper bound for unseen
  entries with real scores. A penalty only lowers scores, so every upper bound
  stays valid without change, and pruning is not weakened. The one place that
  counts "strong" hits (`try_skip_scan`) must use penalized scores.
- **Opt in per engine.** `lexical_set_place_ranking(engine, true)` before
  sealing. File catalogs turn it on (daemon writer, CLI query, benchmark,
  quality evaluator). The desktop catalog leaves it off. A delta segment
  inherits the setting from its reference engine, so an entry scores the
  same in a delta as after a full rebuild, and segments still merge by raw
  score.
- **Seal-time one-symbol answers** run through the normal query path after
  the penalties exist, so they include them.

### Tests

- `test_place.c`: every marker, case folding, Go module and cache names,
  version folders and their near misses (`2024.05.01`, `v1`, `1.`, `.1`,
  `sdkfoo`), level counting, inheritance below a marker, the cap.
- `test_lexical_place.c`: shallow before deep at equal match strength;
  dependency trees after the rest; exact names ordered by place and still
  classified as exact; no tier crossing at the cap; results equal a brute-force
  ranking (place-off scores minus penalty) at capacities 1, 5, 10 and complete,
  for single-word, multiword, one-symbol and boosted queries; a delta segment
  scores exactly like a full rebuild; place ranking off is unchanged.

## Part B: `exclude` setting

### Design

- **Config.** `exclude = PATH` (repeatable, `~/` expands to HOME) is accepted
  by both `torchlight index` and `torchlightd`; unknown keys are still
  rejected. Values are canonicalized like `root` and `allow`; a missing path
  stays as written.
- **Crawl.** `crawl_create` takes a list of excluded scopes instead of the
  single state directory. Every excluded scope wins, even below a root or an
  allowlisted directory, exactly as the state directory does today, so one
  decision function still serves walks, scoped walks and coverage checks.
- **Pruning.** An excluded subtree is not reported, so the next full scan
  (the CLI's `index`, or the daemon's startup check) deletes its saved
  entries, like any path that disappeared. Excluded folders are never
  watched. Like `root` and `allow`, a changed configuration takes effect when
  the daemon restarts.

### Tests

- `test_crawl.c`: an excluded subtree is skipped; exclusion beats the
  allowlist; a root inside an exclusion reports nothing; `crawl_covers` and
  `crawl_scope` respect exclusions; several exclusions at once.
- `test_cli.py`: `index` skips an excluded tree, re-indexing after adding
  `exclude` prunes it, an invalid value fails with its line number.
- `test_daemon.py`: the daemon never returns excluded paths.

## Measurements and reference ranges

Each number is measured on this machine for the new build and HEAD, with
noisy numbers repeated alternating the builds. Results go into a health
report in the style of the M7 Phase 3 report.

| Measure | Reference range |
|---|---|
| Typing p95 at 500k (`make bench`) | ≤ 5 ms and within the HEAD runs' spread |
| Typing p95 at 50k | ≤ HEAD + 10% |
| Engine build time at 500k | ≤ HEAD + 5% |
| Engine memory at 500k | ≤ HEAD + 1% |
| Benchmark ranking fixtures | 9/9 |
| Synthetic held-out recall@10 | reported; any drop above 0.02 explained |
| Mixed relevance evaluator | its own gates pass |
| Live catalog, 50 queries: third-party in top 10 | lower than HEAD |
| Live catalog, 50 queries: own in top 10 | higher than HEAD |
| Query round trip at 213k (scenarios) | p50 ≤ 2 ms, p95 ≤ 5 ms |
| Full check at 213k | ≤ 1,000 ms (no change expected) |
| Steady RSS at 213k | ≤ HEAD + 2 MB |
| Unit, integration, sanitizer and lint suites | all pass, zero warnings |

## Steps

1. `place` module and its tests.
2. Lexical integration, CLI/writer/benchmark/evaluator opt-in, tests.
3. `exclude` in crawl, CLI and writer, tests.
4. Measurements against HEAD; health report.
5. ADR 0047, module docs, glossary, evaluation, system design page,
   milestone status.

## Out of scope

- Learning folder affinity from history (folder credit). It needs a
  per-folder weight instead of per-entry boosts; see the discussion that led
  to this plan.
- Recency from modification time.
- Default exclusions beyond today's `node_modules`, `target`, `build` and
  `__pycache__`: a penalty keeps third-party files findable.

## Results (2026-10-12)

### Ranking on the live catalog

`tests/quality/place_eval.py` with the HEAD and M8 engine libraries on a
read-only dump of the live catalog (213,356 entries), 50 everyday queries,
top 10 each (500 results):

| Measure | HEAD | M8 |
|---|---|---|
| Third-party results in the top 10 | 315 | 192 |
| Own-folder results in the top 10 | 143 | 225 |
| Other results (cloned libraries and the like) | 42 | 83 |
| `sc`: own results in the top 10 | 0 | 7 |
| `scripts`: rank of `~/workspace/torchlight/scripts` | below 10 | 5 |
| Queries with fewer own results | | 1 (`model`, 10 to 8) |

`model` lost two own results seven folders deep
(`AndroidStudioProjects/.../cpp/include/model.h`) to shallower matches of
the same strength: the intended trade-off. Queries whose best matches are
exact names inside third-party trees (`readme`, `doc`, `util`, `build`,
`license`) keep them first: a penalty never crosses the exact-name tier, and
`exclude` is the remedy there.

### Labeled synthetic quality (`make bench`, held-out queries)

| Corpus | recall@1 HEAD / M8 | recall@10 HEAD / M8 | MRR@10 HEAD / M8 |
|---|---|---|---|
| 50k synthetic | 0.461 / 0.451 | 0.605 / 0.633 | 0.507 / 0.508 |
| 500k synthetic | 0.372 / 0.373 | 0.539 / 0.541 | 0.423 / 0.426 |
| 213k real home mirror | 0.492 / 0.463 | 0.632 / 0.634 | 0.531 / 0.517 |

These known-item queries target a uniformly random file. On the real mirror
84% of files are in the Android SDK and the Go module cache, exactly the
files M8 demotes, so recall@1 drops by 0.029 there (abbreviations: recall@10
0.515 to 0.445). recall@10 holds on every corpus. The benchmark's 9 ranking
fixtures pass on every run.

### Typing latency (`make bench`)

| Corpus | HEAD p95 | M8 p95 |
|---|---|---|
| 50k synthetic (3 runs each) | 0.55 to 0.65 ms | 0.55 to 0.58 ms |
| 213k real home mirror (2 runs each) | 2.81 to 2.94 ms | 2.78 to 2.82 ms |
| 500k synthetic (6 runs each, mirrored order) | median 4.75 ms (4.19 to 4.95) | median 4.77 ms (4.52 to 4.93) |

The first implementation stored one penalty per directory node. Every scored
entry then needed two scattered loads (its parent node, then the node's
penalty), and at 500k typing p95 rose to a 4.91 ms median (p50 0.92 against
0.87 ms). Storing the penalty per entry (2 bytes, 1 MiB at 500k) brought it
to 4.77 ms, inside HEAD's own spread, with p50 0.90 ms. Engine build time and
engine memory did not change measurably (build 3.0 to 3.4 s for both).

### Daemon, exclusions and tests

- **Daemon on the 213k mirror** (two runs per build): search ready after a
  restart 1,522 / 1,557 ms (HEAD 1,552 / 1,496), query p95 idle 2.28 / 2.96 ms
  (HEAD 2.77 / 2.93), full check median 1,001 / 1,018 ms (HEAD 1,307 / 1,048),
  steady RSS 163 MB (HEAD 162).
- **Mixed fixture:** the evaluator's gate passes against the frozen baseline
  (held-out nDCG@10 0.939 to 1.000); all 44 top-10 lists equal HEAD's.
- **Exclusions:** excluding `~/Android/Sdk` and `~/go/pkg/mod` indexes 34,989
  of 213,287 entries (0.7 s instead of 4.0 to 4.4 s from scratch, 14.8 MB
  instead of 90.4 MB on disk) and, on the live catalog, leaves 64 third-party
  and 311 own results in the top 10 (HEAD with the same exclusions: 116 and
  272).
- **Tests:** `make test` and `make lint` pass. Eleven of twelve deliberately
  broken rules make a test fail; the twelfth, the score floor of 1, cannot be
  reached because the weakest matches score about 270, above the 256 cap.
