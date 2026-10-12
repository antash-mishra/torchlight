# M7 plan: quiet indexing and lean memory

**Status: Phase 1 complete (2026-10-11): every gate passes on a realistic
desktop after the [real-desktop review](#real-desktop-review-2026-10-11).
Phase 2 implemented (2026-10-11): every exit criterion passes, with three
open red flags; see [Phase 2 results](#phase-2-results-2026-10-11) and the
[health report](reports/m7-phase2-health-report.html). Phase 3 implemented
(2026-10-12): the three red flags are fixed; see
[Phase 3 results](#phase-3-results-2026-10-12) and its
[health report](reports/m7-phase3-health-report.html).** This is
the working plan for two phases of indexing work, based on a headless
profile of the daemon taken on 2026-10-10. Raw data is in
`tests/bench/results/2026-10-10-m7-baseline.jsonl` (scenario metrics) and
`tests/bench/results/2026-10-10-m7-baseline-profile.txt` (profiles, system
calls, I/O, memory). Phase 1 results are in
[Phase 1 results](#phase-1-results-2026-10-10) below, ADR 0038 and ADR 0039.
The [Phase 1 review fixes](#phase-1-review-fixes) close the repair and metadata
persistence gaps. The [review rerun](#phase-1-review-results-2026-10-11) records
the deletion-compaction experiment, which was then
[removed at the user's request](#deletion-compaction-removed-2026-10-11).

## In simple words

Torchlight already uses inotify. When a file appears, disappears or is
renamed, the daemon rescans only the folder that changed and the result is
searchable about 0.1 s later. inotify itself is cheap: its share of the
daemon's memory is about 4 to 5 MB for 34k watched folders.

The cost comes from three other things:

- **The 30-second full rescan.** Every 30 seconds the daemon walks every
  indexed file, checks each one twice, sends every row through SQLite
  (a lookup, an upsert and a "seen" marker each) and then rolls the
  transaction back because nothing changed. This costs about
  9% of a CPU core around the clock on a 213k-file home folder, and about
  22% at 500k files. It also writes about 100 MiB to a temporary file each
  time.
- **Rewritten files rescan their whole folder.** A program that saves or
  appends to one file (a log, an editor autosave, a download) makes the
  daemon rescan every entry in that folder and publish a new index, even
  though search only uses names. One file written 20 times a second in a
  2,838-entry folder costs 43% of a core.
- **Freed memory is not given back.** After big changes, glibc keeps freed
  index memory. RSS stayed at 434 MB after a burst of changes; one
  `malloc_trim` call brought it back to 223 MB.

Phase 1 stops doing work that changes nothing. Phase 2 makes the full scans
that remain (startup, overflow repair, explicit `reconcile`) cheap.

## What we measured (2026-10-10)

### Setup

- **Machine:** Intel i7-8700K (6 cores, 12 threads, AVX2), 32 GB, Linux 6.8,
  ext4 on LVM.
- **Daemon:** a headless, isolated `torchlightd` per run: its own database,
  config, socket and `HOME`/`XDG_*`, lexical only, `--no-history`. Driven
  over IPC by a Python harness.
- **Build:** `-O3 -DNDEBUG -g -fno-omit-frame-pointer
  -mno-omit-leaf-frame-pointer`.
- **Profiler:** `perf` is unavailable on this machine
  (`perf_event_paranoid=4`), so an `LD_PRELOAD` sampler was used. It arms a
  per-thread `CLOCK_THREAD_CPUTIME_ID` timer at 500 Hz (one sample per 2 ms
  of that thread's CPU), walks frame pointers and is symbolized with `nm`.
  Time spent inside a system call is charged to its libc wrapper (`statx`,
  `pread64`, ...). Thread names are thread start routines: `worker` is the
  writer's indexing thread, `persistence_worker` the SQLite owner,
  `search_worker` the search thread and `main` the IPC loop.
- **System calls and I/O:** `strace -f -c -w`, and `strace -y` for file
  targets.
- **Corpora,** real files on disk:
  - **home:** an empty-file mirror of a real home catalog: 213,286 entries,
    34,471 directories, mean path 96 bytes.
  - **fx500:** the M1 synthetic fixture (`bench_fixture 500000`): 494,261
    entries, 41,283 watched directories.
- **Noise:** the user's resident `-O0` daemon kept re-indexing the real home
  folder during all runs. CPU and wakeup figures come from per-thread
  `/proc` counters and are not affected by it. Latency figures carry that
  noise.
- **Units:** memory figures are `/proc` values in MiB, written MB, as in
  earlier plans.

### Results by scenario (home mirror, 213k, unless noted)

| Scenario | Time / latency | CPU | Memory |
|---|---|---|---|
| Cold index (empty database) | 6.4 s | 5.6 CPU-s (indexing 3.1, persistence 2.4) | RSS 226 MB, peak 274 MB |
| Cold index, fx500 | 14.6 s | 12.9 CPU-s | RSS 391 MB, peak 491 MB |
| Warm start (saved database) | socket refuses connections for 1.5 to 1.6 s, then a 3.8 to 4.3 s startup full rescan | 5.9 CPU-s (catalog load 1.6 on `main`) | RSS 232 MB |
| **Idle, default `--rescan-ms 30000`** | 2 full rescans in 95 s | **8.7% of a core** | RSS climbs 232 → 243 MB |
| Idle, periodic rescans off | 0 rescans | 0.1% of a core; 41 wakeups/s (IPC loop 20/s, indexing thread 20/s) | flat 232 MB |
| Live `-O0` daemon on the real home folder | 298 of 379 passes were full rescans over about 3 h | 8.5% of a core (60 s sample) | RSS 209 MB |
| **One no-change full rescan** | 3.9 s | **4.1 CPU-s** | RSS 232 → 243 MB over two rescans |
| One no-change full rescan, fx500 | 13.2 s | 9.7 CPU-s | 405 MB |
| Create one file, small folder | p50 108 ms, p95 532 ms (one outlier in 15) | | |
| Create one file, 2,838-entry folder | p50 170 ms, p95 184 ms | | |
| Delete one file (small / big folder) | p50 107 / 172 ms | | |
| 20,000 files in 2,000 new folders (written in 0.8 s) | last file searchable 3.5 s after the writes; 3 scoped passes, **2 full engine rebuilds** | 4.3 CPU-s | **peak 548 MB**, RSS after 402 MB |
| Rename that 22k-entry tree | 2.5 s; 1 full rebuild | 2.6 CPU-s | peak 584 MB, RSS after 433 MB |
| Delete that tree | 0.24 s; no rebuild | 0.5 CPU-s | |
| **One file appended and closed 20×/s for 30 s, 2,838-entry folder** | 197 folder rescans, 197 snapshot publications | **13.1 CPU-s = 43% of a core** (66 ms per write) | |
| Queries, idle (900 queries) | round trip p50 0.83, p95 2.84, p99 4.47 ms | | |
| Queries during a full rescan | p50 0.89, p95 3.25, p99 4.75 ms | | |
| **`malloc_trim(0)` after the events above** | | | **RSS 434 → 223 MB** (12 malloc arenas) |
| `malloc_trim(0)`, fx500 after one rescan | | | 405 → 372 MB |

The release build does not make rescans cheaper. The `-O3` daemon idles at
8.7% of a core with 30 s rescans and the user's `-O0` daemon at 8.5%. The
time goes to system calls and SQLite, which compiler flags don't change.

### Where the CPU goes in a no-change full rescan (profiler, 213k)

From the 95 s idle profile (two rescans), per rescan:

| Thread | Work | CPU-s | Share |
|---|---|---|---|
| persistence | `store_put` for every entry: identity `SELECT`, upsert attempt, `INSERT INTO seen` (about 640k statements) | 2.06 | 50% |
| persistence | prune: `DELETE … WHERE path NOT IN (SELECT path FROM seen)` (`run_scoped`) | 0.27 | 7% |
| indexing | `fts` stat of every entry (`fstatat64`) | 0.56 | 14% |
| indexing | our second stat of every entry (`statx` in `file_identity`) | 0.52 | 13% |
| indexing | `getdents64` | 0.18 | 4% |
| indexing | `inotify_add_watch` into a second, new inotify instance | 0.08 | 2% |
| indexing | rest of the walk: opens and closes, `report`, batch copies, `malloc`/`free` | about 0.3 | 8% |
| other threads | | 0.03 | <1% |

At fx500 the split is the same, scaled: persistence 6.2 CPU-s (`store_put`
4.65, prune 0.55, `pread64` 0.92) and indexing 3.3 CPU-s (`fstatat64` 1.27,
`statx` 1.06). The two threads run one after the other, and the remaining
3.7 s of the 13.2 s wall time is spent off-CPU. The sampler sees only CPU time;
the temp-file I/O below is the likely cause.

### System calls and file I/O per no-change full rescan (213k)

From `strace -c` (two full walks, halved) and an isolated `strace -y` trace
of one forced rescan:

| Call | Count | Note |
|---|---|---|
| `newfstatat` | about 213,000 | `fts` stats every entry |
| `statx` | about 213,000 | `file_identity` stats every entry again, by full path |
| `getdents64` | about 69,000 | |
| `openat` / `close` / `fstat` | about 34,500 each | directory opens |
| `inotify_add_watch` | 34,471 | all watches re-added to a new instance; 68,942 kernel watches exist until the swap |
| `pwrite64` to `/var/tmp/etilqs_*` | 25,694 = **100.4 MiB** | SQLite temp file: the `seen` temp table spills |
| `pread64` from `/var/tmp/etilqs_*` | 47,869 = **187.0 MiB** | same temp file read back |
| `pread64` from `catalog.db` | 47,067 = 183.9 MiB | 123 MB database through the default 2 MB page cache |

At the 30 s default, that is about 100 MiB of temp-file writes every 34 s.

### Where the CPU goes on events (profiler)

- **Churn** (197 writes to one file in the 2,838-entry folder): 13.1 CPU-s.
  - **Indexing thread, 5.3 s.** Walking the folder took 4.70 s: `fstatat64`
    1.42, `statx` 1.40 and `inotify_add_watch` 1.08. The last one re-adds the
    watches of that folder's subdirectories on every pass.
  - **Persistence thread, 7.8 s.** `store_put` took 4.73 s; pruning, commit
    and WAL I/O took the rest.
  - **Publication is cheap:** `lexical_finish` is 0.02 s. The waste is the
    folder rescan and the SQL, done for a change that cannot alter any search
    result.
- **20k-file burst:** 4.27 CPU-s, 3.38 s of it in two full rebuilds
  (`publish_full`):
  - `lexical_finish` 2.65 s, of which `qsort_r` 1.60 s (glibc merge sort,
    `compare_keys`/`compare_occurrences`)
  - `prefix_finish` 1.06 s
  - `typo_finish` 0.42 s
  - `store_load_catalog` 0.74 s (every row reloaded from SQLite)
  - `build_entry` 0.47 s

  The rename shows the same shape: 1.75 of its 2.61 CPU-s are one full
  rebuild. These costs belong to later work (see Out of scope).
- **Queries during a full rescan:**
  - The search thread spent 1.70 CPU-s on 900 queries, with the Frizbee
    matcher hottest at 18.8% of samples.
  - The rescan barely moves query latency (p95 2.84 → 3.25 ms). The rescan
    costs throughput and power, not interactivity.

### Memory

| Part | Size at 213k | How measured |
|---|---|---|
| Steady daemon RSS | 226 to 232 MB | `/proc` after idle |
| Lexical engine | 168 MB (about 830 B per entry, for 96 B paths) | `bench_lexical --paths` on the same paths |
| inotify watcher, in the daemon | about 4 to 5 MB (34,471 × entry, path copy, map slot) | computed from `watch.c` structures and mean path 83 B |
| inotify, kernel side (not in RSS) | at most about 1 KB per watch by the kernel's own accounting, so ≤ 35 MB; mostly keeps directory inodes cached | kernel `INOTIFY_WATCH_COST` |
| Freed but retained heap after big changes | about 210 MB | RSS before and after `malloc_trim(0)` |
| Peak during a full rebuild | 548 to 584 MB | `VmHWM` reset before the event |

Conclusion: inotify is the right mechanism and is cheap. The memory to win
back is the allocator's retained heap and, later, the lexical engine.

## Where the code was before M7

| Area | Today | Pointer |
|---|---|---|
| Periodic repair | Every `rescan_ms` the worker sets `full`, so every root is crawled; default 30 s | `src/service/writer.c:976` (`worker`), `src/bin/torchlightd.c:23` |
| Event → work | Every event adds the parent folder as a children-only scope; `created` makes it recursive | `writer.c:360` `add_parent_scope`, `writer.c:367` `changed` |
| Event kinds | `tl_watch_event` has `is_dir`, `overflow`, `created`; write/attribute events look like any other change | `include/torchlight/watch.h:11`, `src/fs/watch.c:193`, mask at `watch.c:93` |
| Watcher during full scans | A new inotify instance is built and every directory re-added, then swapped in | `writer.c:698` `prepare_watch`, `writer.c:714` `swap_watch` |
| Crawl | `fts_open(FTS_PHYSICAL \| FTS_NOCHDIR)` stats each entry, then `file_identity` runs `statx` on the full path again | `src/fs/crawl.c:194`, `crawl.c:99` |
| Persistence of a scan | `store_put` per entry: identity `SELECT`, upsert, `INSERT INTO seen`; prune by `NOT IN seen` | `src/storage/store.c:413`, `store.c:433`, temp tables at `store.c:261` |
| Change tracking | SQLite update hook counts every `files` update, including mtime/size only, as a catalog change | `store.c:66` `catalog_change` |
| Snapshot reclaim | Retired snapshots are freed on the writer thread; nothing returns memory to the OS | `src/index/catalog.c:538` `catalog_reclaim`, called at `writer.c:983` |

## Target behavior after M7

```
inotify event
  ├─ entry created / deleted / moved ─────► scoped rescan (parent, or subtree
  │                                          for created dirs) → delta publish
  ├─ file written or attributes changed ──► nothing (search reads names only)
  ├─ directory attributes changed ────────► recursive rescan of that directory
  │                                          (it may have become readable)
  └─ overflow / drain failure ────────────► full repair scan

timers and signals
  ├─ coverage gaps (unwatched directories,
  │   network/FUSE mounts, offline or
  │   unreadable roots) ──────────────────► rescan only those scopes, every rescan_ms
  ├─ mount table changed ─────────────────► full repair scan
  └─ healthy ─────────────────────────────► backstop full scan every repair_ms (1 h)

full repair scan (startup, overflow, reconcile, backstop)
  one statx per entry → compare with persisted state in memory →
  write only differences → watches added to the live instance
```

## Phase 1: stop paying for work that changes nothing

### Step 1.0: scenario benchmark in the repo

Every later step needs a before/after number from the same scenarios.
`bench_daemon.py` runs with `--rescan-ms 3600000`, so it never measured
periodic cost.

- Add `tests/bench/bench_scenarios.py`. It materializes a corpus as empty
  files on disk (the synthetic fixture, or `BENCH_PATHS=file` mirrored like
  the home corpus above) and runs one isolated headless daemon per phase. It
  records:
  - cold index and warm start
  - idle with periodic rescans on and off
  - single-file create/delete lag in a small and a large folder
  - 20k-file burst, rename and delete
  - a 20 Hz churn
  - query latency idle and during a forced full rescan
  - RSS and peak RSS

  CPU and wakeups come from per-thread `/proc` counters. The client
  reconnects, because the daemon closes clients idle for 5 s
  (`IPC_DEADLINE_MS`).
- Add the sampler as a developer tool: `tests/bench/profiler/sampler.c` plus
  `analyze.py`, built only by a `make profile-daemon` target. It gives
  per-thread profiles on machines where `perf` is locked. It is not linked
  into any shipped binary.
- `make bench-scenarios` writes `tests/bench/results/<date>-m7-*.jsonl`.

Exit criteria: the harness reproduces this document's baseline within run
noise; `make lint` covers the new C file.

### Step 1.1: repair scans only where inotify can miss changes

Goal: a healthy daemon never walks everything on a timer.

- Keep full scans at startup, on overflow or drain failure, for explicit
  `reconcile`, when the scope set overflows (`WRITER_SCOPE_CAPACITY`), for
  uncovered scopes (`resolve_scopes`) and after failed passes. Today's
  triggers stay.
- Track a **repair set** of scopes where inotify can miss changes:
  - directories whose `watch_add` failed (`TL_LIMIT`/`TL_IO`), recorded in
    `scan_entry`
  - offline roots and unreadable directories (already counted)
  - mount points of filesystems without reliable local events. The crawl
    sees `st_dev` change between a directory and its parent; `statfs` then
    flags NFS, SMB/CIFS, FUSE, 9p, Ceph and AFS.
  - no watcher at all (instance creation failed), which keeps today's full
    periodic scan
- Every `rescan_ms` (default stays 30 s), rescan only the repair set, through
  the existing scoped path with recursive scopes. If the set is too large,
  fall back to a full scan, as today.
- When the repair set is empty, run one **backstop** full scan every
  `repair_ms`. New flag `--repair-ms N`, default 3,600,000, `0` disables.
- Watch `/proc/self/mountinfo` (`POLLPRI`) in the writer's poll set. A mount
  table change schedules a full scan: a remounted root is back, or a new
  mount appeared under a root.
- `IN_ATTRIB` on a directory schedules a recursive rescan of that directory.
  Today a directory that becomes readable gets only a children-only rescan of
  its parent, so its contents wait for the next periodic full scan.
- Status gains `repair_scopes` and `last_full_reason` (startup, overflow,
  reconcile, backstop, mount, failure) so the cause of each full scan is
  visible.

Preserve: overflow and capacity fallbacks, offline/unreadable rows kept,
restart repair, `torchlight reconcile`.

Tests:
- Daemon test: a healthy daemon with a short `--rescan-ms` performs no full
  scan in an idle window.
- With `--watch-capacity` forced small, a change in an unwatched subtree is
  found within `rescan_ms` by a scoped repair, not a full scan.
- Unit test for filesystem-type classification.
- `IN_ATTRIB` on a formerly unreadable directory indexes its contents.
- The mount signal goes through an injectable descriptor in writer options
  so tests can fire it.

ADR 0038 records this. It replaces ADR 0011's "periodic scans (default 30
seconds)" rule.

Expected: idle CPU at 213k from 8.7% to 0.2% of a core or less, and no
temp-file I/O while idle.

### Step 1.2: metadata-only events neither rescan nor publish

Goal: rewriting or touching a file costs nothing; search reads only names.

- **`watch`:** add `bool metadata` to `tl_watch_event`. It is set for
  `IN_CLOSE_WRITE` and `IN_ATTRIB` on non-directories. Creates, deletes and
  moves are unchanged.
- **`writer` `changed`:** metadata events add no scope.
- **`store`:** in `upsert`, `identity_changed` already reads the existing
  row. When the row exists and its identity is unchanged, the update only
  touches mtime/size. Record it as `metadata_changed`, not as a catalog
  change: no change id, no `changed`.
- **Commit and publish:** `apply_batch` commits when names or metadata
  changed. Only name changes publish. This also stops scans that refresh
  mtime/size from publishing empty deltas.
- **Embedding columns:** `PUT_SQL` clears them when mtime or size changes.
  Embeddings are of path text, so keep them when only metadata changed.
  Today this matters only for the parked semantic path.

Decision (see below): ignore metadata events entirely (recommended), or
refresh the one file's row without rescanning its folder.

Tests:
- `watch` unit: close-write and attrib on a file set `metadata`; on a
  directory they don't.
- `store` unit: a metadata-only upsert commits and reports no catalog
  change; replacement, rename, insert and delete still report changes.
- Daemon test: appending to a file 50 times causes no scoped pass and no
  publication, while a newly created file in the same folder is still
  searchable within the usual lag.

ADR 0038 covers this too.

Expected: the 20 Hz churn scenario from 43% to under 1% of a core, and 197
publications to 0.

### Step 1.3: give freed memory back

Goal: RSS returns near steady state after rebuilds and bursts.

- **Reclaim result:** `catalog_reclaim` returns how many snapshots it
  destroyed. Small API change, documented in the header.
- **Release hook:** add `void (*release_memory)(void *context)` to
  `tl_writer_options`, passed through `tl_daemon_options`. After a reclaim
  that freed a base snapshot (a full build retired), the writer calls it at
  most once per second. The library stays allocator-neutral; `torchlightd`
  supplies a glibc wrapper around `malloc_trim(0)` (`#ifdef __GLIBC__`).
- **Allocator policy:** set it in `torchlightd`'s `main`, the only global
  process policy, which keeps it out of library code:
  - `mallopt(M_MMAP_THRESHOLD, …)` so large engine arrays are allocated
    directly from the OS and returned when freed. A fixed threshold also
    stops glibc from raising it after the first large free.
  - `M_ARENA_MAX` to cap arenas: 12 were observed. The values are chosen by
    measurement.
- **Measure** build time and query latency with each setting. Large arrays
  served directly from the OS fault in on build.

Tests: a writer test with a counting hook sees one release after a full
rebuild's old snapshot is reclaimed and none for a delta publication.

ADR 0039 records the allocator policy.

Expected at 213k: RSS after burst, rename and churn from 434 MB to 250 MB or
less, with steady RSS unchanged. Peak RSS during rebuilds is later work.

### Phase 1 exit criteria

- 10 minutes idle at 213k: at most 0.2% of a core, zero full scans while
  coverage is complete; `last_full_reason` shows startup only.
- Churn scenario: at most 2% of a core, zero publications.
- RSS after burst, rename and churn within 10% of steady RSS. Redefined by
  ADR 0044 after the [real-desktop review](#real-desktop-review-2026-10-11):
  against a warm steady state, a trim returns at most 5 MiB of free heap after
  the events, and RSS once the base is rebuilt is within 10%.
- Create/delete lag unchanged (small folder p50 ≤ 120 ms).
- Repair still works: an unwatched-subtree change, an overflow and a
  remounted root are each picked up.
- `make test`, `make lint` clean. Module docs (watch, writer, store, catalog,
  daemon, cli for new flags), glossary terms (repair set, backstop scan,
  metadata event), ADRs 0038/0039 and a recorded `bench-scenarios` run.

### Phase 1 results (2026-10-10)

Same machine and 213k home mirror, `bench_scenarios.py` (Step 1.0) with the
`-O3` frame-pointer build. "Before" is the harness re-run of the baseline
binary (`2026-10-10-m7-harness-baseline.jsonl`), which reproduced the numbers
above within run noise (idle 6.9% against 8.7% of a core, churn 44% against
43%, RSS 412 against 434 MB after events). "After" is
`2026-10-10-m7-phase1.jsonl`; allocator variants are in
`2026-10-10-m7-allocator.jsonl`.

| Scenario | Before | After |
|---|---|---|
| Idle, default settings | 2 full scans in 95 s, 6.9% of a core, 151 MiB written | 10 min: 0 full scans, **0.09%** of a core, 0 MiB written, `last_full_reason: startup` |
| Idle, rescans off (floor) | 0.08%, 41 wakeups/s | 0.10%, 41 wakeups/s |
| 20 Hz churn, 2,838-entry folder | 191 scoped passes, 191 publications, **44%** of a core | 1 pass and 1 publication (creating the file), **0.4%** |
| Steady RSS (warm start) | 214 MB | **168 MB** |
| Cold index peak / burst peak / rename peak | 260 / 473 / 521 MB | 220 / 371 / 374 MB |
| RSS after burst, rename, delete and churn | 412 MB (`malloc_trim`: 197) | 192 MB (`malloc_trim`: 189) |
| Create lag, small / large folder (p50) | 108 / 162 ms | 108 / 161 ms |
| Query round trip idle (p50 / p95) | 0.71 / 2.48 ms | 0.75 / 2.20 ms |
| Cold index, warm-start scan | 6.2 s, 3.2 s | 6.3 s, 3.4 s |

Exit criteria:

- **Idle:** met. Ten minutes at 0.09% of a core with no full scan.
- **Churn:** met. 0.4% of a core; the one publication is the churned file's
  own creation, which is a new name.
- **Create/delete lag:** met (small-folder p50 108 ms).
- **Repair:** met by tests: an unwatched subtree (`test_writer_repair.c`,
  `test_daemon.py`), a replayed overflow (`test_writer.c`), a mount-table
  signal (`test_writer_repair.c`) and a root that comes back
  (`test_daemon.py`).
- **RSS after events within 10% of steady:** not met as measured, 192 MB
  against 168 MB (+14%). Freed-but-retained heap is gone (a final
  `malloc_trim` frees 3 MB, against 215 MB before). The rest is live index
  data: after the burst (10% more entries) RSS is 181 MB (+8%); the rename's
  full rebuild then builds a base of 235k entries, and deleting the tree only
  tombstones 22k of them, because removals do not count toward the delta's
  compaction bound (ADR 0030). The base keeps them until the next compaction.
  Counting tombstones toward that bound would release about 20 MB at the cost
  of a full rebuild (about 2 CPU-s at 213k) after large deletions. The review
  below tried that policy and measured its cost; it was subsequently removed.

Measured along the way:

- A real mount-table change on this desktop during one run forced a full
  scan (`last_full_reason: mount`). A 15-minute recording afterwards saw none,
  so these are occasional here.
- `malloc_trim(0)` costs about 8 ms per call at 213k.
- The allocator policy chose a fixed 1 MiB `M_MMAP_THRESHOLD` and no arena
  cap (ADR 0039).

## Phase 1 review fixes

**Repair and metadata fixes retained; deletion optimization withdrawn.**
The review of `4701519` reproduced two gaps
despite `make test` and `make lint` passing. The recorded RSS result also
missed the existing acceptance gate. The first two fixes remain implemented
with regressions. The deletion optimization was removed after measuring its
cost, so the original 10% memory gate remains open. Phase 2 remains separate.

| Finding | Reproduced behavior | Required result |
|---|---|---|
| Repair coverage lost after a failed walk | With two watch slots, `root/parent/child` is a repair scope. Removing `parent`'s permissions drops `repair_scopes` from 1 to 0. Restoring permissions and creating a file under `child` leaves it undiscovered across ten repair intervals; explicit reconciliation finds it. | Keep retrying the failed scope and discover changes after access returns. |
| Metadata transaction discarded | A file grows from 1 to 9 bytes; explicit reconciliation leaves its stored size at 1 and its old mtime unchanged. | Commit metadata changes without publishing a catalog snapshot. |
| Deleted base entries retain memory | Recorded RSS after burst, rename, deletion and churn is 192 MiB versus 168 MiB steady (+14.3%); the target is at most +10%. | Open: meet the original RSS gate without the withdrawn immediate deletion-compaction policy. |

### Fix 1: preserve repair coverage across failed scans

**Files:** `src/service/writer.c`, `tests/unit/test_writer_repair.c`,
`tests/test_daemon.py`, and the writer module documentation.

The reviewed `scan_scopes` called `settle_repairs` before crawling. If
`crawl_scope` returned `TL_IO` before reporting any entries (for example,
`realpath` could not traverse a parent), nothing put the repair scope back.

- Keep the current rediscovery of repair scopes during a successful
  recursive walk, but on `TL_IO` retain or re-add the attempted directory as
  a recursive repair scope before releasing its path. Cover failed
  children-only event scans as well: they also leave unverified contents.
- Preserve the existing rule that a failed scan cannot prune stored rows.
  Retry at `rescan_ms`; use the existing full-scan fallback when the repair
  set overflows or allocation fails.
- A failed walk is not evidence that a directory was deleted. Remove stale
  repair paths only after a successful containing-scope scan confirms their
  absence, or a successful full scan reconstructs coverage. Ensure restoring
  access clears repairs once the directory is successfully watched; a scope
  still exceeding watch capacity must remain in the repair set.
- Keep status consistent with retained repair scopes. Attribute filtering
  must not be the only way a failed scope can resume: periodic repair must
  recover it even when its parent generates no useful event.

**Regression:** reproduce the two-slot case with `rescan_ms=100` and
`repair_ms=0`. Wait for a repair pass while the parent is unreadable, assert
the repair remains and existing entries remain resolvable, restore access,
then verify a new child file becomes searchable through scoped repair with
only the startup full scan. Use bounded polling of counters and paths,
rather than timing-only assertions. Run the permission case as a non-root
user; a root-only test run cannot establish this acceptance result. Also
cover repeated failed walks, successful recovery, and cleanup of a repair
path that was actually deleted.

**Done when:** access loss cannot silently empty the repair set; recovery
works without explicit reconciliation or the backstop, and healthy idle
still schedules no passes.

### Fix 2: commit metadata without publishing

**Files:** `src/service/writer.c`, `tests/test_daemon.py`, relevant store and
writer tests, and their module documentation.

- In `apply_batch`, read both `store_catalog_changed` and
  `store_metadata_changed`, checking each status.
- Commit if either flag is true; roll back only when neither is true.
  Keep the output `changed` flag restricted to catalog changes so
  `publish_changes` is never called for a metadata-only commit.
- Preserve transaction rollback and retry on errors. Keep embedding columns
  and file ids for unchanged identities. Allow the stored `catalog_gen` to
  advance without changing the published one, as ADR 0038 specifies.

**Regression:** index a file, change its size and mtime without changing its
name or identity, and force reconciliation. Read the committed database
from a separate connection and verify the new values persisted while the
published `catalog_gen`, file id, `delta_publications` and `full_builds`
remain unchanged. A second unchanged reconcile must not advance the stored
`catalog_gen`. Then create a sibling and verify ordinary publication still
works across that gap in stored and published `catalog_gen` values. Cover
identity adoption for a legacy row and retain the existing embedding and
replacement tests. Database assertions belong in the integration tests or
through the store API; production SQL stays in `store.c`.

**Done when:** metadata discovered by scans persists once, causes no
publication, and does not interfere with later catalog changes. Ordinary
writes and file attribute events must still schedule no scan.

### Fix 3: compact retained deletions

**Withdrawn.** Implemented in `33d0000`, then removed at the user's request
because its bulk-deletion cost outweighed the measured memory saving.
[ADR 0041](adr/0041-compaction-counts-retired-base-entries.md) retains the
original decision and measurements as history. ADR 0040 remains reserved by
Phase 2 below.

The restored compaction threshold counts live delta entries only. Deletions
still hide rows through tombstones, while their base memory remains until
a later rebuild. A future memory fix needs separate design and measurement;
deferred compaction is not implemented by this removal.

### Validation and completion

1. Keep the repair and metadata regressions, which failed on the reviewed
   code before their fixes. After removing deletion compaction, require large
   and accumulated deletions to remain incremental, live delta growth to
   retain its bound, and old/new snapshot readers to stay consistent.
2. Run `make test` and `make lint`. Run `make bench` to check query latency
   and ranking through the resulting snapshot layouts. Keep existing
   allocation, rename/replacement, overflow, offline-root, watch-exhaustion,
   publication-failure and semantic lifecycle checks passing.
3. In `bench_scenarios.py`, create and index the churn file before starting
   its counters and CPU measurement. This isolates writes from the file's
   creation, so the zero-publication gate is literal. Retain the burst,
   rename, delete and post-release RSS measurements; measure memory before
   any diagnostic manual trim. Run the same harness against the baseline
   and fixed binaries with separate run directories.
4. Record a full `bench-scenarios` run on the same 213k home mirror, with
   `--idle-s 600`, and a distinct results label. Compare idle, churn,
   create/delete lag, query latency, steady/peak RSS and rebuild cost. Use
   the 500k synthetic corpus as a scale check, not as a substitute for the
   213k acceptance corpus. Missing corpus or machine access leaves the
   corresponding measurement gate open.
5. Require all of the following before marking Phase 1 complete:

   | Gate | Required evidence |
   |---|---|
   | Repair correctness | Failed-scan recovery regression passes without a full scan; existing overflow, mount-signal and offline-root checks pass. |
   | Metadata persistence | Refreshed metadata commits once; no metadata-only snapshot publication; a subsequent name change publishes correctly. |
   | Healthy idle | 10 minutes at 213k, at most 0.2% of a core, no full or scoped passes after startup while coverage is complete. |
   | Churn | 20 Hz writes to an already indexed file, at most 2% of a core, zero passes and publications. |
   | Memory | After burst, rename, delete and churn settle and leases drain, RSS is at most 1.10 times that run's steady RSS, without manual trimming. Superseded by ADR 0044: retained free heap at most 5 MiB, and RSS after a rebuild within 10% of the warm steady RSS. |
   | Responsiveness | Small-folder create/delete p50 stays at most 120 ms; report query percentiles and compaction-related deletion latency against baseline. |
   | Repository checks | Tests and lint pass; allocation-free query checks and ranking checks remain intact; `CLAUDE.md` and `AGENTS.md` match. |

6. Update this plan's results, `docs/evaluation.md`, the affected module
   Status sections, ADRs 0038/0039 and the compaction ADR, `PLAN.md`,
   `docs/README.md`, and `docs/milestone-status.md` with the actual outcome.
   Align the system overview/design pages with the verified status. Keep
   existing measurements as history and record new results separately.
   The RSS gate is open again after the requested removal. Keep it explicit
   until a separate memory improvement meets it; do not relax the 10% target.

## Phase 1 review results (2026-10-11)

**Historical results for `33d0000`, before the deletion optimization was
removed.** These numbers describe that version, not current memory acceptance.

All three fixes were implemented in that version. The repair, metadata and
deletion-trigger regressions failed on the reviewed code before the fixes.
`make test` passed with ASan/UBSan, including non-root permission cases, allocation-free
queries, publication failure recovery and semantic lifecycle checks.
`make lint` passes, and `CLAUDE.md` and `AGENTS.md` match.
`make bench` passes all nine ranking fixtures at both 50k and 500k. Typing
p95 is 0.505/4.117 ms; at 500k it is 4.267 ms with the full 2048-item usage
summary. Whole-query p95 is 0.812/5.274 ms. These lexical measurements ran
after the scenario benchmarks and do not include IPC.

The scenario harness was rerun against the saved `4701519` binary and the
fixed `-O3` profile build in separate directories. Both use the same empty-file
home mirror (178,569 files and 34,452 directories, including the same
2,838-entry busy folder). The original NUL path list was reconstructed from
the previous mirror's catalog; no file contents were copied. The churn file
is now created and indexed before counters start. The reference desktop had
background load; tests/lint and the synthetic scale run also ran during the
ten-minute idle observation. Timings are single runs, not evidence of a query
speedup. The normal installed daemon was left running.

| Check at 213k | Reviewed `4701519` | Fixed | Acceptance |
|---|---:|---:|---|
| Idle, default settings, 600 s | Earlier run: 0.09% CPU | 0.09% CPU; zero full/scoped scans and publications; no measured I/O | Pass: at most 0.2% |
| 20 Hz writes to an indexed file, 30 s | 0.07% CPU; zero passes/publications | 0.13% CPU; zero passes/publications | Pass: at most 2% |
| Steady / post-event RSS, before trim | 166 / 190 MiB (+14.5%) | 167 / 181 MiB (+8.4%) | Pass: at most +10% |
| Small-folder create / delete p50 | 107.6 / 106.8 ms | 107.5 / 107.2 ms | Pass: at most 120 ms |
| Busy-folder create / delete p50 | 161.7 / 160.3 ms | 160.8 / 160.9 ms | Recorded; unchanged scale |
| Idle query round trip p50 / p95 / p99 | 0.746 / 2.520 / 3.856 ms | 0.753 / 2.113 / 3.372 ms | 900 requests per run |
| During full scan, query p50 / p95 / p99 | 0.815 / 2.698 / 4.274 ms | 0.753 / 2.673 / 4.389 ms | 900 requests per run |

The memory gate is met before the optional diagnostic trim (181 → 179 MiB).
It comes with a measured large-deletion cost:

| 213k event | Reviewed full builds / CPU / visibility lag / peak RSS | Fixed full builds / CPU / visibility lag / peak RSS |
|---|---|---|
| Create 20k tree | 2 / 3.72 s / 3240 ms / 368 MiB | 2 / 3.56 s / 2938 ms / 368 MiB |
| Rename tree | 1 / 1.96 s / 2047 ms / 370 MiB | 1 / 2.00 s / 2014 ms / 377 MiB |
| Delete tree | 0 / 0.38 s / 181 ms / 189 MiB | 2 / 3.40 s / 3052 ms / 371 MiB |

The deletion crosses the compaction bound twice across three scoped batches.
No full filesystem scan runs for these events. Visibility lag starts after
the filesystem action; the deletion itself takes 315 ms in the fixed run.
Small changes remain incremental. The large-deletion cost was initially accepted as the
tradeoff for releasing retired base entries under the original RSS target
([ADR 0041](adr/0041-compaction-counts-retired-base-entries.md)).

The 500k synthetic scale check ends at 326 MiB versus 319 MiB steady (+2.2%)
before trim. Deletion causes one full build, takes 4.00 CPU-s and 3284 ms to
become visible, with a 679 MiB deletion peak (685 MiB during rename).
Small-folder create/delete p50 is 108.7/107.3 ms. Idle query round trip
p50/p95/p99 is 1.467/6.455/11.876 ms; during a full scan it is
1.379/6.536/12.557 ms. This scale check ran alongside repository validation;
it supplements the 213k acceptance run and makes no isolated latency claim.

Raw results (file dates reflect the start of this run):

- [Reviewed binary, corrected harness](../tests/bench/results/2026-10-10-m7-review-baseline.jsonl)
- [Fixed, all 213k phases including ten-minute idle](../tests/bench/results/2026-10-10-m7-review-fixed.jsonl)
- [Fixed, 500k scale check](../tests/bench/results/2026-10-10-m7-review-fixed-500k.jsonl)
- [`make bench` output](../tests/bench/results/2026-10-10-m7-review-lexical.txt)

The fixed acceptance invocation was:

```sh
python3 tests/bench/bench_scenarios.py --daemon build/profile/torchlightd \
  --sampler build/profile/sampler.so --paths /tmp/torchlight-m7-review-corpus.paths \
  --work build/m7-fixed --label review-fixed --idle-s 600 \
  --results tests/bench/results/2026-10-10-m7-review-fixed.jsonl
```

That version met the memory gate; the removal below reopens it. Phase 2 still
covers full-scan cost and reuse of deleted watch slots, and large rebuild peaks
remain a documented limitation.

## Deletion compaction removed (2026-10-11)

The user requested removal after reviewing the measured tradeoff: 9 MiB less
post-event RSS at 213k cost 3052 ms deletion visibility lag instead of 181 ms,
and two full builds instead of none. The extra tombstone accounting and its
catalog projection API are removed. The previous live-delta compaction rule
is restored, without adding a replacement optimization or timer.

Repair retries, metadata-only commits, the existing allocator policy and the
release hook remain implemented. Updated delta and writer regressions require
deletion batches above the live-entry threshold to stay incremental and
preserve old readers, while live delta growth still compacts when over its
bound. The original 10% post-event RSS acceptance gate is open again.

The removal was checked with `make test` (ASan/UBSan, including the non-root
permission and metadata regressions) and `make lint`. `CLAUDE.md` and
`AGENTS.md` still match. A fresh `-O3` scenario run reused the same 213k
empty-file mirror and working directory after tests/lint finished; the cold
phase reset its isolated database. It ran cold indexing, update lag and the
event sequence. The earlier ten-minute idle and 500k scenario measurements
were not repeated for this removal.

| Check after removal | Measured result |
|---|---|
| Delete 20k tree | 4 delta publications, zero full builds or full scans; 85 ms visibility lag after the 330 ms filesystem deletion; 0.42 CPU-s; 189 MiB peak |
| Small-folder create / delete p50 | 108.3 / 106.8 ms |
| 20 Hz writes for 30 s | 0.10% of a core; zero passes and publications |
| Steady / post-event RSS before manual trim | 167 / 191 MiB (+14.4%): misses the 10% gate |

This is one run under normal desktop load; event batching and latency vary.
The deterministic regressions establish that deleted base rows no longer
force compaction, while the scenario confirms its practical cost is gone.
The optional diagnostic trim reduced RSS to 187 MiB; acceptance uses the
191 MiB measurement before it.

`make bench` also passes all nine ranking fixtures at 50k and 500k. Typing
p95 is 0.566/4.430 ms, whole-query p95 is 0.892/5.347 ms, and 500k typing
with the full 2048-item usage summary is 4.590 ms. These engine measurements
exclude IPC and ran after the scenario benchmark.

Results: [removal scenarios](../tests/bench/results/2026-10-11-m7-deletion-rollback.jsonl)
and [`make bench`](../tests/bench/results/2026-10-11-m7-deletion-rollback-lexical.txt).
The scenario invocation was:

```sh
python3 tests/bench/bench_scenarios.py --daemon build/profile/torchlightd \
  --sampler build/profile/sampler.so --paths /tmp/torchlight-m7-review-corpus.paths \
  --work build/m7-fixed --label deletion-rollback \
  --results tests/bench/results/2026-10-11-m7-deletion-rollback.jsonl cold lag events
```

## Real-desktop review (2026-10-11)

**Phase 1 complete on a realistic desktop.** An audit of the live daemon
(the user's `-O0` build, two hours on the real home folder) found it idling at
1.59% of a core while the benchmark reported 0.09%. The benchmark had hidden
two costs: its daemons ran with an empty `XDG_DATA_DIRS` and `--no-history`,
and its idle window never spanned the top of an hour.

| Finding (live daemon, 213k entries) | Cost | Fix |
|---|---|---|
| The desktop catalog re-parsed all 171 `.desktop` files (about 1 MB of localized keys) every second | 1.39% of a core, 87% of the idle total; about 1.6 MiB/s of reads | Parse only when a stat change stamp moves ([ADR 0042](adr/0042-desktop-change-stamps.md)) |
| Timeshift's hourly cron mounts its backup disk under `/run/timeshift/` at :00:01 and unmounts it at :00:18; each change caused a full scan | 2 full scans an hour: about 6 CPU-s and 95 MiB written per hour; the backstop never ran | Full scans only for mounts at, below or above a root ([ADR 0043](adr/0043-mount-changes-near-roots-and-watch-slot-reuse.md)) |
| Dead watch slots counted against capacity until a full scan replaced the watcher | Latent: fewer full scans let directory churn exhaust the watcher | Reuse slots on `IN_IGNORED` (ADR 0043) |
| The memory gate compared post-event RSS with RSS read before any query | 7 MiB of the 24 MiB gap was first-touched query scratch | Warm baseline and rebuild check ([ADR 0044](adr/0044-m7-memory-acceptance.md)) |

What changed:

- **Desktop change stamps.** Every second the desktop worker stats each
  `.desktop` entry (dev, inode, mode, size, mtime, ctime) through the same walk
  discovery uses, and parses only when that stamp differs from the stamp of
  what its last parse read. It also parses on `reconcile`, 5 s after the last
  change, and every 10 minutes (for `TryExec` binaries appearing on `PATH`).
- **Mount relevance.** After each mount-table signal the writer compares
  `mounts` snapshots and schedules a full scan only for a mount at, below or
  above a root. Status counts every signal in `mount_changes`.
- **Watch slot reuse.** Slots of dropped watches are freed and reused, and
  their descriptors leave the lookup map (`hashmap_remove`).
- **Thread names.** `tl-index`, `tl-persist`, `tl-search`, `tl-desktop`,
  `tl-parallel` and `tl-semantic`, so profiles, `top -H` and the benchmark
  attribute CPU per component.
- **Realistic benchmark.** `bench_scenarios.py` now gives its daemons the
  host's applications and history by default (`--desktop empty` and
  `--no-history` restore the old setup), reports `cpu_s_by_thread`, warms up
  before reading steady RSS, measures the memory gates of ADR 0044, and adds a
  `mounts` phase that mounts tmpfs in a private user and mount namespace.

### Results (213k mirror, `-O3`, host applications, history on)

Same machine and the same 178,569-file, 34,452-directory mirror as the
earlier M7 runs, with the host's 167 `.desktop` files
([raw results](../tests/bench/results/2026-10-11-m7-real-desktop.jsonl)):

| Gate | Measured | Acceptance |
|---|---|---|
| Idle, default settings, 600 s | 0.15% of a core (desktop checks 0.06%, indexing tick 0.05%, IPC tick 0.04%); zero scans, publications and writes; 41.3 wakeups/s | Pass: at most 0.2% |
| 20 Hz writes to an indexed file, 30 s | 0.13%; zero passes and publications | Pass: at most 2% |
| Small-folder create / delete p50 | 107.4 / 106.9 ms | Pass: at most 120 ms |
| Free heap a trim returns after the events | 3 MiB | Pass: at most 5 MiB |
| RSS after the rebuild against warm steady | 185 / 176 MiB (+5.1%, with 8,008 entries added to force the rebuild) | Pass: at most +10% |
| 20 tmpfs mounts and unmounts outside the root | 40 changes seen, zero full scans | Pass |
| One mount and unmount inside the root | two full scans, `last_full_reason: mount` | Pass |

Measured along the way:

- Query round trip idle p50/p95/p99 0.78/2.32/4.01 ms, during a full scan
  0.78/2.65/4.24 ms: unchanged from the earlier runs.
- Steady RSS 169 MiB cold and 176 MiB warm; 195 MiB after the events
  (+10.8% against warm steady, +15.4% against cold). The rest of that gap is
  the deleted tree's 22k entries in the base engine (deleted-entry retention,
  ADR 0044), released by the rebuild.
- Event costs are unchanged: the 20k burst became visible after 2.9 s with
  two full builds, the rename after 2.1 s with one, the deletion after 203 ms
  with none; rebuild peaks reach 371 to 381 MiB.
- Each remaining full scan costs about 3.5 CPU-s and pushes about 150 MiB
  through SQLite (the warm-start scan wrote 72 MiB; the two mount scans 297
  MiB together). The hourly backstop alone adds about 0.09% of a core
  averaged over an hour. Phase 2 targets exactly this.

The same harness on the baseline binary (HEAD `f339518`, five-minute idle;
[raw results](../tests/bench/results/2026-10-11-m7-real-desktop-baseline.jsonl))
shows what the fixes changed:

| Scenario | Baseline | Fixed |
|---|---|---|
| Idle, default settings | 1.58% of a core, 286 MiB read in 5 min | 0.15%, 1 MiB read in 10 min |
| 20 tmpfs mount cycles outside the root | 4 full scans, 13.5 CPU-s, 288 MiB written | zero full scans, 0.03 CPU-s |
| Memory gates (trim / after rebuild) | 3 MiB / +6.2% | 3 MiB / +5.1% |

Memory is unchanged by these fixes; it passed ADR 0044's gates before them,
which is what the decomposition predicted. `make bench` passes all nine
ranking fixtures at 50k and 500k, with 500k typing p95 4.313 ms
([output](../tests/bench/results/2026-10-11-m7-real-desktop-lexical.txt)).
`make test` (ASan/UBSan, including the non-root desktop cases) and
`make lint` pass; `CLAUDE.md` and `AGENTS.md` match.

On the real home folder the two binaries ran side by side as isolated
daemons (own databases and sockets, the live session's environment, history
on) from 16:20 to 17:03, while repository work caused nine scoped passes in
each, then crossed Timeshift's 17:00 check:

| Real home folder (213k entries) | Baseline | Fixed |
|---|---|---|
| 38 minutes idle with ordinary file activity | 1.87% of a core (desktop thread 1.70%) | 0.25% (desktop checks 0.08%; most of the rest is the scoped passes) |
| Timeshift mount at 17:00:01 and unmount at 17:00:18 | two full scans, 10.5 CPU-s, `last_full_reason: mount` | no full scan; `mount_changes` 0 to 2; 0.49 CPU-s over the same three minutes |

The invocations were:

```sh
python3 tests/bench/bench_scenarios.py --daemon build/profile/torchlightd \
  --sampler build/profile/sampler.so --paths /tmp/torchlight-m7-review-corpus.paths \
  --work build/m7-fixed --label real-desktop --idle-s 600 \
  --results tests/bench/results/2026-10-11-m7-real-desktop.jsonl
python3 tests/bench/bench_scenarios.py --daemon <baseline>/build/profile/torchlightd \
  --sampler build/profile/sampler.so --paths /tmp/torchlight-m7-review-corpus.paths \
  --work build/m7-fixed --label real-desktop-baseline --idle-s 300 \
  --results tests/bench/results/2026-10-11-m7-real-desktop-baseline.jsonl \
  cold warm events mounts
```

## Phase 2: make the remaining full scans cheap

After Phase 1, full scans still run at startup, after overflow, for
`reconcile`, as the hourly backstop and for degraded coverage. Each costs
3.9 s and 4.1 CPU-s at 213k, and 13.2 s and 9.7 CPU-s at 494k. Most of
that is checking each file twice and writing every unchanged row to SQLite.

### Phase 2 in simple words

Torchlight keeps a card for every item on disk, so search can answer
without touching the disk. Phase 1 made it rely on inotify: when a file is
created, deleted or renamed, only that folder is checked again. Some moments
still need a walk over every item, a **full scan**. Phase 2 does not make
full scans rarer. It makes each one cheap.

A full scan that finds nothing changed costs about 4 s, 4 CPU-s and about
150 MB of file traffic at 213k items, and 13 s at 494k. Almost all of it is
wasted:

| Waste | Cost per no-change scan at 213k |
|---|---|
| Every file is inspected twice (`fts` stat, then our `statx`) | about 1.1 CPU-s (27%) |
| Every unchanged file still goes through SQLite: about 640k statements, then a rollback | about 2.3 CPU-s (57%) |
| The `seen` temp table spills to a scratch file | about 100 MB written and 187 MB read back |
| All 34k folder watches are added to a new watcher, then swapped in | little CPU, but the kernel watch count doubles during the scan |

**How often full scans run.** Unchanged by Phase 2:

| Trigger | When | Frequency |
|---|---|---|
| Startup | the daemon starts | once per login, boot or restart |
| Backstop | `--repair-ms` (default 1 h) after the last full scan | at most once an hour |
| Overflow | inotify lost events (its queue holds 16,384 by default) | only during bulk changes: large checkouts, unpacking, syncs |
| Too many folders | more than 1,024 changed folders in one pass, or a root itself changed | bulk changes |
| Mount | a mount at, below or above a root | rare; mounts elsewhere no longer count (ADR 0043) |
| `reconcile` | the user asks | manual |
| Failure | retry after a failed pass | rare |
| Degraded coverage | no watcher, or more than 256 directories inotify cannot cover | **every `rescan_ms` (30 s)** while it lasts |

On a healthy desktop that is one scan per start plus at most one an hour:
about 25 a day on an always-on machine. The live daemon on 2026-10-11 showed
exactly that state (`last_full_reason: startup`, `repair_scopes` 0, 34,474
watches).

What Phase 2 changes per scan, and so on average:

| | Today | After Phase 2 (expected) |
|---|---|---|
| No-change full scan, 213k | 3.9 s, 4.1 CPU-s | about 1 s, 1.1 CPU-s |
| Startup scan, 213k | 3.8 to 4.3 s | 1 to 1.5 s |
| No-change full scan, 494k | 13.2 s | about 3 s |
| Scratch-file traffic per scan | about 290 MB | none |
| Hourly backstop, averaged | about 0.1% of a core | about 0.03% |
| Degraded coverage (a full scan every 30 s) | about 14% of a core | about 4% |
| Memory | | +5 MB at 213k, +12 MB at 494k |

Phase 2 lowers the number of full scans in one case only, indirectly: no
longer doubling kernel watches during a scan makes it less likely that a
large tree hits the per-user watch limit (249,237 here, shared with editors
and IDEs). Hitting that limit is what drops the daemon into degraded mode,
which runs a full scan every 30 s. The backstop interval stays at 1 h: after
Phase 2 it costs too little to be worth lengthening.

Users notice the daemon settling about 1 s after login instead of about 4 s,
and faster recovery after bulk changes. Search speed does not change.

### Step 2.1: one stat per entry

- Open the walk with `FTS_NOSTAT`. glibc's `fts` then skips the stat for
  entries whose `d_type` is known and not a directory, reporting `FTS_NSOK`.
  Directories are still stat'd for cycle detection.
- `report`/`file_identity` take all metadata from the single `statx`. Under
  `FTS_NOSTAT`, glibc stats into a local buffer and leaves `fts_statp` NULL
  for **every** entry, directories included (checked with glibc 2.39 on
  2026-10-11). So `device_boundary`, the mount-point check that sends
  network and FUSE mounts to the repair set, must compare the `statx` device
  with the parent directory's, kept per level by the walk.
- `DT_UNKNOWN` filesystems fall back to today's behavior automatically.
- Follow-up only if measurements justify it: a small `openat`/`getdents64`
  walker using `statx(dirfd, name)`, which avoids full-path lookups for deep
  paths.

Tests: existing crawl tests (unreadable entries, symlinks not followed,
`FTS_NS`/`FTS_ERR`, raw-byte names). Add a case checking that a regular file
gets exactly one stat. Count calls through an injected stat in tests, or
assert on the produced records.

Expected: about 179k fewer `fstatat` per 213k scan, roughly 0.45 CPU-s. The
same saving applies to every scoped rescan.

### Step 2.2: reuse the live watcher

- Full scans add watches into the live instance, as scoped scans already do,
  instead of building a replacement and swapping.
- `watch` gains a path → slot lookup. For a known, live directory,
  `watch_add` skips the system call and only marks the slot as seen in this
  scan generation. A new or relocated path calls `inotify_add_watch`, which
  returns the existing descriptor for an already watched inode, and updates
  the stored path.
- After a successful full scan, `watch_sweep` removes slots not seen in this
  generation with `inotify_rm_watch`. These are directories now excluded or
  missed as deleted.
- A new instance is created only when none exists (startup, or a retry after
  instance failure).
- This also removes the 1.08 s of `inotify_add_watch` seen in the churn
  profile, because scoped walks re-add watches for subdirectories they list.

Preserve: the existing watch set stays live throughout (ADR 0012), cookie
pairing, relocation on directory moves, capacity and exhaustion counting.

Tests:
- Adding the same directory twice yields one slot and one descriptor.
- Sweep removes a stale slot.
- Repeated full scans keep `watches` constant and create no new instance
  (count through `create_watch`).
- Excluding a directory removes its watch after the next full scan.

ADR 0040 records this. It replaces ADR 0012's build-then-swap rule.

Expected: per full scan, 34,471 `inotify_add_watch` calls drop to the number
of new directories. The transient double of kernel watches (68,942) is gone.
That matters against the per-user limit of 249,237 shared with editors and
the per-watcher capacity of 65,536.

### Step 2.3: compare in memory, write only differences

- **Persisted state:** the indexing thread keeps a compact map of what is
  persisted: path hash → `{id, signature}`. The signature hashes identity
  (dev, inode, birth or ctime), mtime, size and `is_dir`. It costs about
  24 B per entry: about 5 MB at 213k and 12 MB at 500k. It is loaded with
  the catalog at startup and updated from each committed batch. Paths for
  collision checks are borrowed from the published snapshot
  (`catalog_snapshot_find`).
- **Full scans** put only new or changed entries in the batch. Seen entries
  are marked in a bitmap over map slots. Deletions are the unseen slots
  under successfully scanned roots, excluding kept, unreadable and offline
  scopes, and are sent to persistence as explicit ids.
- **`store`** gains `store_delete_ids`. Full scans no longer use the `seen`
  temp table or `DELETE … NOT IN seen`. Scoped rescans are small and keep
  today's SQL prune. Identity replacement keeps going through
  `identity_changed`/`retire_scope`, now only for changed entries.
- **Batch:** stop copying every crawled path. The batch holds only
  differences.
- **Scoped rescans** use the same comparison for the entries they list.
  Today one file created in the 2,838-entry folder sends all 2,838 siblings
  through `store_put`. That, with statting all 2,838 entries, is why its lag
  is 170 ms instead of 108 ms.
- **Transition:** keep the old path behind a test-only switch so a
  randomized tree test can compare both. Create, delete, replace and rename
  operations must give identical database contents.

Tests:
- The randomized equivalence test above.
- Existing incarnation, rename, offline-root, unreadable-scope and
  crash-recovery tests.
- A no-change full scan executes no `store_put`, checked through store
  change counters.
- Temp-table I/O stays zero: no `etilqs` file is created during a no-change
  full scan.

ADR 0040 covers this with 2.2.

Expected at 213k, together with 2.1 and 2.2: a no-change full scan of about
1 s wall and about 1.1 CPU-s instead of 3.9 s and 4.1 CPU-s. No 100 MiB/187
MiB temp-file traffic. Warm-start rescan of about 1 to 1.5 s instead of 3.8
to 4.3 s. At 494k: about 3 s instead of 13.2 s.

### Step 2.4 (decision after 2.3): skip unchanged directories

Store each directory's `(mtime_ns, ctime_ns)` and list only directories
whose stamp changed, as `mlocate` does. A directory's mtime changes whenever
an entry is added, removed or renamed in it, so names stay exact; file
metadata in unchanged directories is not refreshed. This would cut a
no-change scan to about 34k `statx` calls, but changes what a "full repair"
verifies. Decide with the 2.3 numbers.

### Phase 2 exit criteria

- No-change full scan: ≤ 1.0 s wall and ≤ 1.2 CPU-s at 213k; ≤ 3 s at 494k.
- No SQLite temp-file I/O during scans; kernel watch count never exceeds the
  steady count during a full scan.
- Warm-start scan ≤ 1.5 s at 213k.
- Equivalence test passes; all existing crawl, watch, store, writer and
  daemon tests pass.
- `make test`, `make lint` clean. Module docs for crawl, watch, store and
  writer, ADR 0040, and a recorded `bench-scenarios` run comparing against
  this baseline.

### Implementation plan (2026-10-11)

Reading the steps against the current code found gaps the steps above do
not cover. Each must be handled in its step.

**Step 2.1 (small):**
- `report` returns `TL_IO` when `fts_statp` is NULL, so under `FTS_NOSTAT`
  it must stop reading `fts_statp` entirely. Mount-point detection moves to
  `statx` data (see the step). Without that move, network and FUSE mounts
  silently lose their repair scans.
- `file_identity` falls back to the `fts` stat when `statx` is unavailable
  (`ENOSYS`, `EINVAL`, `EOPNOTSUPP`). Under `FTS_NOSTAT` there is no such
  stat, so that fallback becomes an `fstatat` call.

**Step 2.2 (medium):**
- Skip `inotify_add_watch` only when both the path and the directory's
  identity (device and inode, which the crawl already reports) match the
  slot. A directory moved out of every root keeps its watch and its old path,
  because only `IN_MOVE_SELF` reaches its own watch. A new directory created
  under the old name would then match by path alone and stay unwatched.
- `watch_degraded` is `unavailable != 0`, a cumulative counter. Today it
  clears because each full scan replaces the watcher. A reused watcher needs
  a per-generation count, or one failure would report degraded forever.
- `prepare_watch`/`swap_watch` and `building_watch` go away. A new instance
  is created only when none exists. `coverage_lost` clears once one does.

**Step 2.3 (large; the main saving):**
- Refresh the persisted-state map from SQLite after each commit, using the
  touched row ids the persistence thread already collects for delta
  publication (`job_ids`, then a `store_load_ids`-style read that also
  returns the signature columns). When those ids are incomplete, reload the
  whole map at the same point the writer already falls back to a full
  rebuild. A failed commit or a `reload` also reloads it. The map is a cache
  of committed rows, never a second source of truth.
- Scoped rescans keep the SQL prune, which deletes every row not in `seen`.
  Unchanged entries in a scoped rescan therefore still need `INSERT INTO
  seen`: one statement instead of three, not zero. To reach zero, read the
  scope's rows by path range and compare them in memory. Do this only if
  measurements justify it.
- `settle_missing_repairs` (through `repair_branch_seen`) checks whether a
  repair directory is gone by searching the batch for every reported
  directory. When the batch holds only differences, the scoped scan must
  record the directories and unreadable entries it reported in a separate
  list.
- Full-scan deletion must reproduce what `PRUNE_SQL`/`FORGET_SQL` give for
  free:
  - kept scopes (unreadable directories, offline roots)
  - registered roots nested inside another root
  - roots removed from the configuration
  - renames applied before and after the upserts
  - `identity_changed` replacement
- No schema change is needed: `mtime`, `size`, `is_dir` and `identity` are
  already stored.

**Order of work**, measuring after each step:
1. Add a `rescan` phase to `bench_scenarios.py` that forces no-change full
   scans. It records wall time, CPU per thread, `etilqs` file traffic and
   peak kernel watches. Record the baseline on the current build.
2. Step 2.1.
3. Step 2.2, with ADR 0040 (part 1).
4. Step 2.3, in four slices:
   1. Map load and refresh after commit.
   2. Full scans compare against the map and delete by id
      (`store_delete_ids`), with the old path behind a test-only switch and
      the randomized equivalence test.
   3. Scoped rescans use the same comparison.
   4. The batch stops copying unchanged paths.

   ADR 0040 (part 2) covers 2.3.
5. Run the full benchmark against the exit criteria and decide Step 2.4.
6. Update the module docs for crawl, watch, store and writer, the glossary
   (persisted-state map, signature), `milestone-status.md`, `PLAN.md` and
   this plan's results.
7. Validate and write a health report (Step 2.5 below).

### Step 2.5: validation and health report

After implementation, check the whole system the way a lab checks a blood
sample: many measurements, each with a reference range and a flag.

- **Live service.** Rebuild, restart the user's `torchlightd` service and
  confirm it is healthy on the real home folder: the startup scan, status
  counters, the watch count, idle CPU and RSS.
- **Correctness.** `make test` (ASan, UBSan and LeakSanitizer), `make lint`,
  the randomized equivalence test, and `make bench` (ranking fixtures and
  typing latency at 50k and 500k).
- **Performance.** `bench_scenarios.py` on the baseline binary (HEAD before
  Phase 2) and on the Phase 2 binary, back to back on the same mirror:
  - no-change full scans
  - warm start
  - idle
  - events
  - mounts
  - queries during a scan

  Profile both binaries with the sampler, and use `strace -c` for system
  calls and `etilqs` traffic.
- **Memory.** Run the sanitized daemon through the scenarios and stop it
  cleanly to look for leaks. Record steady and peak RSS, retained free heap
  (`mallinfo`, trim), the persisted-state map's size and whether RSS grows
  across repeated full scans.
- **Red flags.** Any value outside its reference range, any regression
  against the baseline, any leak, and any test that is flaky or needed a
  retry.
- **Report.** A self-contained local HTML page,
  `docs/reports/m7-phase2-health-report.html`, readable by someone without
  a technical background:
  - a summary verdict
  - one panel per area, with a table of measurement, result, reference
    range, flag and baseline value
  - the red flags, each with a plain-language explanation
  - a definitions section for every technical term used

**Step 2.4 recommendation:** do not adopt it if 2.3 meets the 1 s target.
Full scans are rare after Phase 1. Skipping unchanged directories would save
little, and it weakens what a repair scan verifies.

### Phase 2 results (2026-10-11)

**Phase 2 implemented** ([ADR 0040](adr/0040-cheap-full-scans.md)). The
[health report](reports/m7-phase2-health-report.html) presents every
measurement below with reference ranges, flags and definitions in plain
language. The old (HEAD `ab377a0`) and new optimized builds (`-O3`, sampler
attached) ran back to back on the same machine and trees, with the host's
applications and history on.

| Exit criterion | Measured | Before | Result |
|---|---|---|---|
| No-change full scan at 213k: ≤ 1.0 s, ≤ 1.2 CPU-s | 978 ms (median of 5; runs 968 to 989), 1.00 CPU-s | 3,969 ms, 4.00 CPU-s | Pass, narrowly: the same scan code measured 1,028 ms in an earlier run |
| No-change full scan at 494k: ≤ 3 s | 1,859 ms, 1.89 CPU-s | 7,784 ms, 7.79 CPU-s | Pass |
| No SQLite temp-file I/O during scans | 0 MiB written, 1 MiB read; no `pwrite64` | 293 MiB written, 495 MiB read | Pass |
| Kernel watches never above steady | peak 34,461 = steady | peak 68,914 | Pass |
| Warm-start scan ≤ 1.5 s at 213k | 1,134 ms | 3,251 ms | Pass |
| Equivalence test; all tests; `make test`, `make lint` | 9 of 9 seeds identical; all suites pass; 0 lint warnings | | Pass |

System calls per no-change scan (`strace -c`, three scans): stat calls
430,257 → 249,809 (one `statx` per entry plus fts's own stat of each
directory), `inotify_add_watch` 34,453 → 0, `pread64` 70,026 → 0, `pwrite64`
18,413 → 0. CPU per scan (sampler): database 2.00 → 0 s,
inspecting files 1.00 → 0.56 s, watches 0.17 → 0 s, folder listing and
opening 0.29 → 0.26 s, in-memory comparison 0.10 s.

Unchanged or within noise: idle 0.15% → 0.14% of a core, 20 Hz churn 0.13% →
0.13%, small-folder create/delete p50 107 ms, query round trip p50 0.79 →
0.74 ms (p95 2.46 → 2.80 ms; the query path did not change), `make bench`
9/9 fixtures at 50k and 500k with typing p95 3.74 ms at 500k, mount changes
outside the root 0 full scans, inside 2. Create lag in the 2,838-entry
folder improved from 161 to 139 ms (exact scoped passes). Memory: steady RSS
176 → 171 MiB, after the events 195 → 193 MiB, RSS flat at 199 MiB over 30
consecutive full scans. The ledger itself is 12.6 MiB at 213k. The sanitized
daemon on the 213k mirror (cold index, three forced scans, burst, rename,
delete, queries) reported no leaks, memory errors or undefined behaviour.

Found and fixed while validating:
- **Membership tables restored by rollback.** An unchanged scan rolls back,
  and the `seen`/`kept`/`gone` tables had been emptied inside the
  transaction, so every unchanged scan after a cold index re-deleted and
  journaled them (about 15 MB of temp-file writes per scan at 20k entries).
  They are now emptied outside any transaction; `unchanged_scans_write_nothing`
  in `test_daemon.py` guards it.
- **Ledger refresh before publication** delayed a 22k-entry deletion from
  219 to 344 ms. It now runs after publication. Over 7 repeated runs the
  deletion takes 246 ms against 223 ms, within noise.

Open items (red flags in the report):
- **20k-file burst visible 1.3 s later.** Medians of 7 runs: 3,002 ms against
  1,672 ms. The faster crawl finishes its first pass over the still-growing
  tree earlier, so the second pass exceeds the delta bound and forces a full
  rebuild. The old build's second pass fit a delta in 7 of 8 runs. Total CPU
  is +7%. A tree that appears at once (rename) is +6%. Candidate fix: wait for
  event bursts to settle (up to a cap) before a scoped pass.
- **Search ready after restart 0.2 s later** (1,524 against 1,325 ms, median
  of 6). The startup load now also fills the ledger and reads signature
  columns. Candidate fix: open the socket before the catalog loads (already
  listed under Out of scope), or fill the ledger on the indexing thread.
- **Home-folder rescans cost about 0.1 s of persistence CPU each** (an
  existing cost): `PRUNE_CHILDREN_SQL` for a root walks the whole catalog's
  path range. On the live desktop it dominated idle CPU while this work ran:
  0.44% of a core over 20 minutes and 0.34% over 10 minutes on the final
  build, against 0.25% in the Phase 1 review.
- **Close to their gates:** retained free heap 5 MiB (gate 5) and RSS after a
  rebuild +9.9% (gate 10%), because the ledger grows with the 64k entries the
  rebuild test adds.

The live service (the user's `-O0` unit) restarted on the new build:
- Startup scan 1.23 s.
- Forced full check 2.84 → 1.06 s and 2.94 → 1.07 CPU-s.
- File traffic per check: 182 MiB read and 49 MiB written before; 1 MiB read
  and none written after.
- RSS 170 → 152 MiB.

**Step 2.4 decision:** not adopted. The full scan meets its targets without
it, and skipping unchanged directories would weaken what a repair scan
verifies.

The invocations were:

```sh
python3 tests/bench/bench_scenarios.py --daemon build/m7p2-$BUILD/torchlightd \
  --sampler build/m7p2-base/sampler.so --paths /tmp/torchlight-m7-review-corpus.paths \
  --work build/m7-fixed --label m7p2-$BUILD --idle-s 180 \
  --results tests/bench/results/2026-10-11-m7p2-213k-$BUILD.jsonl \
  cold warm quiet queries lag events mounts rescan syscalls
python3 tests/bench/bench_scenarios.py --daemon build/m7p2-$BUILD/torchlightd \
  --fixture build/bench_fixture --size 500000 --work build/m7p2-fx500 --label m7p2-$BUILD \
  --idle-s 30 --results tests/bench/results/2026-10-11-m7p2-494k-$BUILD.jsonl cold warm rescan
make bench > tests/bench/results/2026-10-11-m7p2-lexical.txt
```

## Phase 3: close the Phase 2 open items (2026-10-12)

The [health report](reports/m7-phase2-health-report.html) left three
results out of range and four at the edge. Phase 3 fixes them without
slowing anything that is fast today.

### Phase 3 in simple words

- **A change directly in the home folder costs 0.1 s of database work.**
  Every rescan of a folder ends with a cleanup query that deletes the rows of
  children that are gone. For the root folder, that query walks every row of
  the catalog. Phase 3 asks the database only about the folder's direct
  children and jumps over their subtrees.
- **A big burst of new files is indexed twice.** The faster scan of Phase 2
  catches a burst halfway, and each half then needs a full rebuild of the
  search index. Phase 3 waits for a burst to settle before rescanning, and
  publishes a large change as a quick update first, rebuilding the index
  afterwards, when nothing else is waiting.
- **Startup got 0.2 s slower** because it fills the ledger. Phase 3 makes
  the ledger cheaper to fill, to look up and to hold.

### Baseline (2026-10-12, HEAD `97deb1d`, 213k mirror, `-O3`)

| Open item | Measured | Reference |
|---|---|---|
| 20k-file burst visible (median of 7) | 3,002 ms | 1,672 ms before Phase 2 |
| Search ready after a restart (median of 6) | 1,524 ms | 1,325 ms before Phase 2 |
| Live desktop background CPU | 0.34% of a core | 0.25% (Phase 1 review) |
| Create or delete one file in the root folder | visible after 230 ms; 250 ms of indexing and database CPU per create/delete pair | 108 ms in a small folder |
| No-change full check | 978 ms | at most 1,000 ms |
| RSS after a rebuild / retained free heap | +9.9% / 5 MiB | at most +10% / 5 MiB |
| Ledger at 213k | 12.6 MiB (about 62 B per row) | about 5 MiB planned |

The root-folder numbers come from a new experiment (files created and
deleted directly in the mirror's root, nine times). Its profile puts
2.43 of 2.51 CPU-s on the persistence thread in `prune_range`, the
`PRUNE_CHILDREN_SQL` statement.

### Step 3.1: prune a folder's children without walking its subtree

`PRUNE_CHILDREN_SQL` deletes every row in the folder's path range whose
first component below the folder was not seen. The range of a root holds
the whole catalog, and the expression runs for every row.

- Walk the range in path order with one prepared statement and step through
  rows while they are direct children. At the first row below a child,
  re-seek past that child's subtree (`child/` up to `child0`). The cost is
  one step per child plus one seek per child that has a subtree, not one
  step per row below the folder.
- Collect the unseen children (including a child known only from rows below
  it), then delete each one and its subtree with the same rules that spare
  kept paths and nested roots. The deleted set is exactly the one the old
  statement deleted.
- Tests: a randomized unit test compares the new prune with a brute-force
  model of the old statement over random trees with tricky names (`a`,
  `a.txt`, `a-b`, `a/x`, orphans below a missing child, kept paths, nested
  roots, the `/` root). A `rootlag` case in the bench harness records lag
  and CPU for changes directly in the root.
- Target: a root-folder create/delete visible within 120 ms, under 10 ms of
  CPU per pass; live idle CPU at or below 0.25%.

### Step 3.2: a lean ledger

- **One-pass fingerprint.** Path key and check come from the two halves of
  one 128-bit hash (MurmurHash3 x64_128, eight bytes at a time) instead of
  two byte-by-byte FNV-1a passes. The store signature hashes its packed
  columns once the same way. Both are in-memory only, never persisted.
- **4-byte index slots.** `hashmap` gains an external-hash mode: slots hold
  only the value (the record index), and the map asks the caller for a
  value's hash when it rehashes or closes a hole. The ledger index drops from
  12 to 4 bytes per slot, and a lookup touches the slot and the record, not
  three arrays.
- **Bulk load.** While a ledger is being refilled (startup, recovery), puts
  only append records; `ledger_seal` builds the index once at its final size
  instead of growing it 13 times.
- Targets: ledger at most 9 MiB at 213k; search ready within 10% of the
  pre-Phase 2 build, measured alternately; no-change full check median at
  most 950 ms.

### Step 3.3: index a burst in one rebuild

- **Settle.** A filesystem event moves the next pass to 100 ms after the
  latest event, but never more than 1 s after the first event since the last
  pass started. A single change is still rescanned 100 ms later; a burst is
  rescanned once it pauses. Requests (`reconcile`, overflow, repair ticks,
  mounts, retries) keep their immediate schedule.
- **Deferred compaction.** A delta may grow past its bound (max(4096,
  base/32)) up to four times that bound when the writer would otherwise
  rebuild in the middle of a burst. The large delta is published at once,
  and the full rebuild (compaction) runs as soon as no event is pending, or
  2 s after the delta first exceeded its bound. Past four times the bound,
  publication rebuilds immediately, as now. Compaction counts as indexing
  in `status`.
- Tests: writer tests for the settle window and its cap, for a delta past
  its bound being published and then compacted, and for compaction waiting
  for pending events.
- Targets: 20k burst visible within 20% of the pre-Phase 2 build (aim:
  about 1.5 s) with one full rebuild; renaming or deleting the 22k tree no
  slower than today.

### What must not regress

Measured on HEAD and on the Phase 3 build back to back, alternating:

- Query round trip p50 and p95 idle and during full scans: within 10%;
  `make bench` 9/9 fixtures at 50k and 500k and typing p95 at most 5 ms.
- Small-folder create and delete p50 at most 120 ms; busiest folder at most
  170 ms; 20 Hz churn at most 2% of a core; idle at most 0.20%.
- Full check at 213k at most 1.0 s and 1.2 CPU-s, at 494k at most 3 s; no
  SQLite temp-file I/O; kernel watches never above steady.
- Cold index within 10%; warm-start scan at most 1.5 s.
- Steady RSS at most HEAD + 2 MiB; peak during the burst at most HEAD;
  retained free heap at most 5 MiB; RSS after a rebuild at most +10%.
- Equivalence test (9 seeds), all unit and integration tests, sanitizers,
  `make lint`.

### Order of work

1. Step 3.1, with its tests and the `rootlag` case; measure.
2. Step 3.2; measure readiness, full check and ledger size.
3. Step 3.3; measure bursts, renames, deletions, lag and queries.
4. Full validation: `make test`, `make lint`, `make bench`, the scenario
   matrix at 213k and 494k for the pre-Phase 2, HEAD and Phase 3 builds,
   profiles, `strace -c`, the sanitized daemon, and the live service.
5. ADR, module docs, glossary, this plan's results, the system design
   page's open items and a Phase 3 health report.

### Phase 3 results (2026-10-12)

**Phase 3 implemented** ([ADR 0045](adr/0045-settled-bursts-and-deferred-compaction.md),
[ADR 0046](adr/0046-children-walks-and-a-lean-ledger.md)). The
[health report](reports/m7-phase3-health-report.html) presents every
measurement with reference ranges and flags in plain language. HEAD
(`97deb1d`, Phase 2) and Phase 3 optimized builds (`-O3`, frame pointers) ran
on the same machine and trees, alternating where a number is a median of
repeats. The pre-Phase 2 build (`ab377a0`) ran for the readiness and burst
references.

| Open item | Phase 3 | HEAD (Phase 2) | Result |
|---|---|---|---|
| 20k-file burst searchable (median of 7) | 435 ms, 1 rebuild, 1.94 CPU-s | 2,974 ms, 2 rebuilds, 3.63 CPU-s | Fixed (pre-Phase 2 today: 2,805 ms, median of 8) |
| Search ready after restart (median of 6) | 1,418 ms | 1,527 ms | Fixed: pre-Phase 2 1,386 ms, so +2.3% (gate +10%) |
| File created or deleted in the root folder | visible after 107 ms, 0 to 10 ms CPU per pair | 236 to 243 ms, 250 to 260 ms CPU | Fixed |
| Live home folder: database CPU per create/delete pair | 0.00 s | 0.24 s | Fixed |
| No-change full check, 213k (median of 5, three rounds) | 943 to 969 ms, 0.97 CPU-s | 993 to 1,021 ms, 1.03 CPU-s | Better, 3 to 6% under the 1 s limit |
| Ledger at 213k / 494k | 8.5 / 19.2 MiB | 12.6 / 27.2 MiB | Better |
| Retained free heap after the events (3 runs) | 5, 5, 5 MiB | 4, 5, 5 MiB | Pass, at the 5 MiB limit |
| RSS after a rebuild vs warm steady (3 runs) | +10.2, +10.8, +10.2% (184 to 185 MiB) | +9.3% (188 MiB) | **Out of range by definition**; see below |

Everything else stayed within noise or improved:
- Queries, three alternating runs at 213k: p50 0.71 to 0.72 ms against 0.72 ms
  idle, p95 2.11 to 2.29 ms against 2.18 to 2.32 ms. During full scans, p95
  is 2.09 to 2.23 ms against 2.07 to 2.10 ms.
- `make bench`: 9/9 fixtures at 50k and 500k. Typing p95 at 500k measured
  4.54 to 4.77 ms against 4.63 to 5.14 ms for HEAD alternately on the same
  loaded machine (Phase 2 recorded 3.74 ms on a quieter one).
- Lag: small folder 107 ms and busiest folder 139 to 140 ms, both unchanged.
  Deleting the 22k tree takes 1 to 159 ms in four scoped passes and 0.25 to
  0.27 CPU-s (was 33 to 222 ms and 0.41 CPU-s). Renaming it takes 434 ms
  (median of 7, was 2,051 ms).
- Queries in the window after a burst while its compaction runs (four bursts):
  p50 0.91 ms, p95 2.69 ms.
- Idle 0.14% of a core (unchanged); 20 Hz churn 0.20 to 0.26% (HEAD 0.23 to
  0.30%); mounts outside the root cause 0 full scans, inside 2.
- Cold index 5.6 s against 6.3 s. Warm start (harness, sampler attached):
  ready 1,544 against 1,639 ms, startup scan 1,093 against 1,159 ms. Steady
  RSS 167 against 172 MiB.
- System calls per full scan are identical. Kernel watches peak at the steady
  34,461. Scans write nothing and use no temp files.
- 494k: full check 1,905 against 1,967 ms; ready 3,303 against 3,493 ms; RSS
  283 against 291 MiB; cold index 12.5 against 12.7 s.
- `make test` (ASan, UBSan, LeakSanitizer, integration suites) and `make lint`
  pass. The sanitized daemon on the 213k mirror (cold index, forced scans,
  burst, rename, delete, root-folder changes, queries) exited cleanly with no
  report.

Profiles (sampler, 213k): in the burst, full rebuilds fall from 3.16 to 1.64
CPU-s (two to one) while delta builds rise from 0.05 to 0.16 CPU-s; the
persistence thread's `prune_range` share (0.18 CPU-s) is gone. At startup the
catalog load takes 0.37 instead of 0.47 CPU-s, and path hashing in the startup
scan falls below 20 ms (was 80 ms).

**The rebuild gate.** ADR 0044 measures RSS after a rebuild against the warm
steady RSS. Phase 3 lowers both: steady by 5 MiB (the smaller ledger) and
after the rebuild by 3 to 4 MiB, so the ratio rises above 10% while the
daemon uses less memory in every state. In a rebuild without the preceding
events, both builds grow about 8% (HEAD 7.7 and 8.3%, Phase 3 8.5 and 7.9%), and
`malloc_trim` afterwards returns at most 1 MiB in either. The extra is
therefore not retained free heap. The gate is left as it is; whether to
restate it in absolute terms (for example, at most the previous build's RSS)
is an open decision.

**Live service** (the user's `-O0` unit, restarted on the Phase 3 build):
- Startup scan 1.10 s.
- Forced full checks 961 to 1,073 ms and 0.98 to 1.07 CPU-s, reading 1 MiB
  and writing nothing.
- RSS 148 to 151 MiB.
- A hidden file created and deleted in the home folder five times costs no
  measurable database CPU (0.24 s per pair before the restart).
- Idle CPU over 10 minutes was 0.16% of a core before the restart and 0.27%
  after it, each window with one scoped pass; a second sample after it read
  0.25% over 15 minutes with six scoped passes. In that sample the desktop
  worker used 0.09% of a core, the main loop 0.06%, the indexing thread 0.06%
  and the persistence thread 0.04%. The first three run no Phase 3 code at
  idle. The persistence thread's share is not explained by the rescans, which
  cost nothing measurable in the probe; its once-a-minute history retention
  is the likely source (the live service cannot be profiled here). This stays an open item (the health
  report flags it): the next step is the desktop worker's once-a-second
  change check and the 41 wakeups per second.

The invocations were:

```sh
python3 tests/bench/bench_scenarios.py --daemon build/m7p3-$BUILD/torchlightd \
  --sampler build/m7p3-$BUILD/sampler.so --paths /tmp/torchlight-m7-review-corpus.paths \
  --work build/m7p3-w$BUILD --label m7p3-$BUILD --idle-s 180 \
  --results tests/bench/results/2026-10-12-m7p3-213k-$BUILD.jsonl \
  cold warm quiet queries lag events mounts rescan syscalls
python3 tests/bench/bench_scenarios.py --daemon build/m7p3-$BUILD/torchlightd \
  --sampler build/m7p3-$BUILD/sampler.so --fixture build/bench_fixture --size 500000 \
  --work build/m7p3-x$BUILD --label m7p3-$BUILD --idle-s 30 \
  --results tests/bench/results/2026-10-12-m7p3-494k-$BUILD.jsonl cold warm rescan
make bench > tests/bench/results/2026-10-12-m7p3-lexical.txt
```

The events phase ran three more times per build
(`2026-10-12-m7p3-213k-events-*.jsonl`) and the queries phase twice
(`2026-10-12-m7p3-213k-queries-*.jsonl`). Medians of 7 bursts and renames,
restart readiness and the root-folder lag came from repeated single-purpose
runs on the same mirror; they are recorded in
`tests/bench/results/2026-10-12-m7p3-repeats.jsonl`.

## Storage alternatives considered: SQLite tuning and mmap

Measured 2026-10-10 on the 213k mirror: three forced no-change full rescans
per variant (`tests/bench/results/2026-10-10-m7-sqlite-pragmas.jsonl`).

| Variant | Rescan wall | CPU per rescan | RSS after |
|---|---|---|---|
| Today | 3.9 to 4.4 s | about 4.1 s | 273 MB |
| `PRAGMA mmap_size=256MiB` (SQLite reads through mmap) | 3.7 to 4.0 s | about 3.9 s | 583 MB (319 MB file-backed) |
| `PRAGMA temp_store=MEMORY` | 3.5 to 3.6 s | about 3.55 s | 472 MB (+200 MB heap) |
| Both | 3.5 to 3.6 s | about 3.65 s | 688 MB |

- **Tuning SQLite's I/O saves 5 to 13% and costs 200 to 400 MB of RSS.** The
  time goes to executing about 640k statements for rows that did not change,
  not to reading pages. Step 2.3 removes those statements, so no pragma is
  adopted.
- **Replacing SQLite with a custom mmap file is rejected.** SQLite holds the
  catalog, history, desktop opens, roots, migrations and the embedding cache.
  It gives crash-safe incremental commits (WAL) that a custom file would
  have to rebuild: a journal, compaction, schema versioning and validation of
  a file the daemon must treat as untrusted input. After Phase 2, SQLite
  work is proportional to real changes, and the query path never touches it.
- **An mmap'd read-only snapshot of the built lexical engine stays a later
  option** (M6 listed it out of scope). It would skip the startup rebuild,
  1.6 CPU-s on `main` at 213k, of which only 0.33 s is reading SQLite. It
  would not shrink the working set. Mapped pages that queries touch count in
  RSS as file-backed memory, as the `mmap_size` run shows, and pages the
  kernel evicts under pressure would turn into page faults on keystrokes.
  The 1.5 s startup gap is fixed more cheaply by opening the socket before
  the catalog loads.

## Decisions to make

Taken (the recommended options; each can still be revisited):

1. **Backstop interval when coverage is healthy:** 1 h, `--repair-ms`
   (`0` turns it off).
2. **Metadata events:** ignored. `IN_CLOSE_WRITE` is no longer subscribed;
   attribute changes of files do nothing, and a directory's only matter when
   it is itself a repair scope.
3. **Flags:** `--rescan-ms` (default 30 s) paces the repair set and
   `--repair-ms` the backstop.
4. **Allocator settings:** `M_MMAP_THRESHOLD` fixed at 1 MiB; `M_ARENA_MAX`
   left at glibc's default (no measured gain).
5. **Name:** M7 is in `PLAN.md` and `milestone-status.md`.
6. **Tombstones and compaction:** the optimization in
   [ADR 0041](adr/0041-compaction-counts-retired-base-entries.md) was withdrawn
   after measuring its large-deletion cost. Compaction again bounds live delta
   entries only. Deleted-entry retention is a documented limit, and the memory
   gate measures retained free heap and RSS after a rebuild against a warm
   steady state ([ADR 0044](adr/0044-m7-memory-acceptance.md)).
7. **Desktop refresh and mount signals:** parse `.desktop` files only when a
   change stamp moves ([ADR 0042](adr/0042-desktop-change-stamps.md)); full
   scans only for mounts near a root, with watch slots reused
   ([ADR 0043](adr/0043-mount-changes-near-roots-and-watch-slot-reuse.md)).

8. **Step 2.4:** not adopted (2026-10-11). The full scan meets its targets
   without it ([Phase 2 results](#phase-2-results-2026-10-11)).

## Out of scope (measured above, planned later)

- **Full rebuild cost and peaks:**
  - `qsort_r` in `lexical_finish` (1.6 s of a 2.65 s rebuild at 213k)
  - `store_load_catalog` reloading every row (0.74 s)
  - two rebuilds per 20k burst
  - peaks of 548 to 584 MB

  Candidates are a radix sort, building from the in-memory base plus delta,
  and deferring compaction until the writer is idle.
- **Warm start:** the socket opens only after the saved catalog loads
  (1.5 s at 213k).
- **Idle wakeups:** 41 per second from two 50 ms poll loops.
- **Lexical engine memory:** 168 MB at 213k, about 830 B per entry. Needs
  per-sub-index accounting in `status` first.
- **The user's database:** 581 MB, including a 109k-row embedding cache
  (106 MiB of payload) kept while semantic search is parked. It costs disk,
  not RAM.
- **fanotify:** whole-filesystem marks need `CAP_SYS_ADMIN`, and
  unprivileged fanotify offers only per-inode marks, like inotify.
