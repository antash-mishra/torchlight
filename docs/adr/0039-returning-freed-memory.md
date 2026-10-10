# 0039. Returning freed memory: trim after big frees, a fixed mmap threshold

- **Status:** Accepted
- **Date:** 2026-10-10
- **Refines:** 0030 (delta publication and compaction)

## Context
The [M7 plan](../m7-plan.md) found that RSS stayed at 412 to 434 MB after a
burst of changes at 213k entries, against a steady 214 MB. One `malloc_trim(0)`
brought it back to about 200 MB: glibc kept the freed memory of retired
engines and scan batches in its arenas. Full rebuilds also peaked at 470 to
520 MB.

Allocator behavior is process-wide, while the library must stay
allocator-neutral and free of global state.

## Decision
- **Release hook.** `tl_writer_options.release_memory` (passed through
  `tl_daemon_options`) is called on the indexing thread, at most once per
  second, after `catalog_reclaim` freed a base engine (it now returns how many
  it freed) or a full scan freed its batch. `torchlightd` supplies a glibc
  `malloc_trim(0)` wrapper; other C libraries get no hook. A trim costs about
  8 ms at 213k.
- **Allocator policy in the executable.** `torchlightd`'s `main` sets
  `M_MMAP_THRESHOLD` to 1 MiB with `mallopt`. Engine arrays of that size and
  more come straight from `mmap` and return to the OS when freed, and a fixed
  threshold stops glibc from raising it after the first large free (its
  default grows from 128 KiB up to 32 MiB).
- **No arena cap.** `M_ARENA_MAX` stays at glibc's default.

Measured on the 213k home mirror (`make bench-scenarios`, results in
`tests/bench/results/2026-10-10-m7-allocator.jsonl`):

| Setting | Steady RSS | Cold index peak | Burst / rename peak | Cold index CPU | Query p95 |
|---|---|---|---|---|---|
| trim hook only | 214 MB | 260 MB | 416 / 464 MB | 5.3 s | 2.7 ms |
| + mmap threshold 1 MiB | 187 MB | 220 MB | 370 / 378 MB | 5.1 s | 2.3 ms |
| + arena max 2 | 214 MB | 268 MB | 417 / 464 MB | (noisy) | 2.6 ms |
| + both | 187 MB | 219 MB | 369 / 377 MB | 5.0 s | 2.4 ms |

With the hook also running after full scans (the startup scan's batch), the
final build idles at 168 MB.

The threshold itself matters little: 256 KiB, 1 MiB and 4 MiB gave steady
RSS of 186, 187 and 189 MB and rebuild peaks of 367 to 378 MB, with 256 KiB
the slowest cold index (5.6 against 5.0 CPU-s). 1 MiB sits in the middle.
Variants were applied with `GLIBC_TUNABLES`, which sets the same parameters
as `mallopt`. During the 1 MiB run a real mount-table change on the test
machine forced a full scan whose freed batch stayed in the heap (+11%); the
hook now also runs after full scans.

## Alternatives considered
- **Trim on a timer.** Wakes an idle daemon for nothing; the writer knows
  exactly when large frees happen.
- **`malloc_trim` inside the library.** Ties a reusable module to glibc and
  makes process policy a side effect of indexing.
- **Capping arenas.** Measured no gain: the trim already covers every arena,
  and fewer arenas would only add contention for the search pool.
- **A different allocator (jemalloc, mimalloc).** A new dependency for what
  two glibc calls achieve.

## Consequences
- Freed heap no longer lingers. After a burst, a 20k-entry rename, its
  deletion and a write churn, a final `malloc_trim` finds 3 MB to free
  instead of 215 MB, and RSS is 192 MB instead of 412 MB. What remains above
  the steady 168 MB is live index data: deleted entries stay tombstoned in
  the base engine until the next compaction (ADR 0030).
- Steady RSS drops from 214 to 168 MB and cold-index and rebuild peaks by 15
  to 28%.
- Large allocations now fault in fresh pages on each build; measured build
  CPU did not grow.
- Peak RSS during full rebuilds is still about twice steady; reducing it is
  later work (radix sort, building from base plus delta).
