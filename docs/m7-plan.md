# M7 plan: quiet indexing and lean memory

**Status: Phase 1 implemented and acceptance verified (2026-10-11).
Phase 2 proposed.** This is
the working plan for two phases of indexing work, based on a headless
profile of the daemon taken on 2026-10-10. Raw data is in
`tests/bench/results/2026-10-10-m7-baseline.jsonl` (scenario metrics) and
`tests/bench/results/2026-10-10-m7-baseline-profile.txt` (profiles, system
calls, I/O, memory). Phase 1 results are in
[Phase 1 results](#phase-1-results-2026-10-10) below, ADR 0038 and ADR 0039.
The [Phase 1 review fixes](#phase-1-review-fixes) and
[acceptance rerun](#phase-1-review-results-2026-10-11) below close the repair,
metadata persistence and memory gaps found in the initial implementation.

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
- RSS after burst, rename and churn within 10% of steady RSS.
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
  below implements that policy and measures its cost separately.

Measured along the way:

- A real mount-table change on this desktop during one run forced a full
  scan (`last_full_reason: mount`). A 15-minute recording afterwards saw none,
  so these are occasional here.
- `malloc_trim(0)` costs about 8 ms per call at 213k.
- The allocator policy chose a fixed 1 MiB `M_MMAP_THRESHOLD` and no arena
  cap (ADR 0039).

## Phase 1 review fixes

**Implemented and verified.** The review of `4701519` reproduced two gaps
despite `make test` and `make lint` passing. The recorded RSS result also
missed the existing acceptance gate. The implementation brief below is
complete, with regressions for each fix and measured acceptance in the next
section. Phase 2 remains separate.

| Finding | Reproduced behavior | Required result |
|---|---|---|
| Repair coverage lost after a failed walk | With two watch slots, `root/parent/child` is a repair scope. Removing `parent`'s permissions drops `repair_scopes` from 1 to 0. Restoring permissions and creating a file under `child` leaves it undiscovered across ten repair intervals; explicit reconciliation finds it. | Keep retrying the failed scope and discover changes after access returns. |
| Metadata transaction discarded | A file grows from 1 to 9 bytes; explicit reconciliation leaves its stored size at 1 and its old mtime unchanged. | Commit metadata changes without publishing a catalog snapshot. |
| Deleted base entries retain memory | Recorded RSS after burst, rename, deletion and churn is 192 MiB versus 168 MiB steady (+14.3%); the target is at most +10%. | Compact after enough base entries are retired and meet the original RSS gate. |

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

**Files:** `src/service/delta.c`, `include/torchlight/delta.h`,
`src/index/catalog.c`, `include/torchlight/catalog.h`, and catalog/writer
tests. The policy is recorded in accepted [ADR 0041](adr/0041-compaction-counts-retired-base-entries.md),
which refines ADR 0030. ADR 0040 remains reserved by Phase 2 below.

Use the existing compaction threshold, but count retained changes correctly:

```
bound = max(min_entries, base_entries / divisor)
affected_entries = projected_unique_base_tombstones + live_delta_entries_absent_from_base
compact when affected_entries > bound
```

The writer's defaults remain `min_entries=4096` and `divisor=32`. A changed
base entry counts once even if it also lives in the delta. A deleted base
entry counts once; repeated changes to that id do not increase the count.
An entry created and deleted entirely within the delta no longer counts.
The current batch's deletions must participate immediately.

- Add a small, allocation-free catalog helper that reports the projected
  tombstone count for a pinned source plus ascending, deduplicated changed
  ids. Validate its inputs and document ownership, errors and the id-order
  contract. The catalog keeps ownership of its bitmap and deduplication
  logic; the delta module does not access snapshot internals.
- In `delta_prepare`, count pending entries absent from the immutable base
  with `lexical_slot`, combine the counts with overflow-safe arithmetic,
  and check the bound before building the delta engine. Preserve existing
  whole-snapshot entry and path-byte limits.
- Return `TL_LIMIT` through the existing writer compaction path. Rebuild
  from the committed catalog, publish, and reset delta bookkeeping only
  after publication succeeds. Compaction itself needs no full filesystem
  scan.
- Reuse snapshot leases, `catalog_reclaim`, and the existing throttled
  release hook. Readers must retain their old view until release; physical
  reclamation follows the last reference. Keep all work on the indexing
  thread and preserve failure recovery.

**Regressions:** check below, exactly at, and above a small threshold;
deletions crossing it in one batch and across several batches; mixed
additions/deletions; repeated updates to the same base id; and creating then
deleting a delta-only entry. Verify small deletions remain incremental,
compaction removes tombstones, surviving paths/ids stay correct, and failed
publication does not prematurely reset bookkeeping. Hold an old snapshot
across compaction and verify it remains usable, then verify its base is
reclaimed and the release hook runs after its last reference goes away.
Use exact snapshot/counter assertions in unit tests; validate RSS with the
scenario benchmark.

**Tradeoff:** large deletions now incur full engine rebuilds (the earlier
213k measurement was roughly 2 CPU-s per rebuild). Record deletion lag,
rebuild count, CPU and peak RSS, including batches that cross the threshold
more than once. Success requires measured memory recovery; the proposed
accounting is not by itself proof of the RSS target.

### Validation and completion

1. Add the repair and metadata regressions first and confirm they fail on
   the reviewed code. Add deletion-compaction regressions that expose the
   current missing trigger, then implement the three fixes with their
   header and module documentation updates.
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
   | Memory | After burst, rename, delete and churn settle and leases drain, RSS is at most 1.10 times that run's steady RSS, without manual trimming. |
   | Responsiveness | Small-folder create/delete p50 stays at most 120 ms; report query percentiles and compaction-related deletion latency against baseline. |
   | Repository checks | Tests and lint pass; allocation-free query checks and ranking checks remain intact; `CLAUDE.md` and `AGENTS.md` match. |

6. Update this plan's results, `docs/evaluation.md`, the affected module
   Status sections, ADRs 0038/0039 and the compaction ADR, `PLAN.md`,
   `docs/README.md`, and `docs/milestone-status.md` with the actual outcome.
   Align the system overview/design pages with the verified status. Keep
   existing measurements as history and record new results separately.
   If the RSS gate still fails, attribute the remaining live/retained memory
   and continue the Phase 1 fix; do not silently relax the 10% target.

## Phase 1 review results (2026-10-11)

All three fixes are implemented. The repair, metadata and deletion-trigger
regressions failed on the reviewed code before the fixes. `make test` now
passes with ASan/UBSan, including non-root permission cases, allocation-free
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
Small changes remain incremental. The large-deletion cost is accepted as the
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

Phase 1 is complete. Phase 2 still covers full-scan cost and reuse of deleted
watch slots; large rebuild peaks remain a documented limitation.

## Phase 2: make the remaining full scans cheap

After Phase 1, full scans still run at startup, after overflow, for
`reconcile`, as the hourly backstop and for degraded coverage. Each costs
3.9 s and 4.1 CPU-s at 213k, and 13.2 s and 9.7 CPU-s at 494k. Most of
that is checking each file twice and writing every unchanged row to SQLite.

### Step 2.1: one stat per entry

- Open the walk with `FTS_NOSTAT`. glibc's `fts` then skips the stat for
  entries whose `d_type` is known and not a directory, reporting `FTS_NSOK`.
  Directories are still stat'd for cycle detection.
- `report`/`file_identity` take all metadata from the single `statx` and no
  longer read `fts_statp` for `FTS_NSOK` entries.
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

Taken for Phase 1 (the recommended options; each can still be revisited):

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
6. **Tombstones and compaction:** [ADR 0041](adr/0041-compaction-counts-retired-base-entries.md)
   counts unique retired base entries and live additions toward the bound.
   The review implementation meets the original 10% RSS gate at +8.4%; its
   extra large-deletion CPU and lag are recorded above.

Still open:

7. **Step 2.4:** adopt directory-stamp skipping or not, after 2.3.

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
