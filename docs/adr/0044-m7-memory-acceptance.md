# 0044. M7 memory acceptance against a warm steady state

- **Status:** Accepted
- **Date:** 2026-10-11
- **Refines:** 0039 (returning freed memory), 0041 (withdrawn deletion
  compaction); replaces the M7 Phase 1 gate "RSS after burst, rename, delete
  and churn within 10% of steady"

## Context
M7 Phase 1 set a memory gate to catch what the original profile found:
after a burst of changes glibc kept about 210 MB of freed engine memory, so
RSS stayed at 412 to 434 MB against 214 MB steady. ADR 0039 fixed that (a
final `malloc_trim` now returns 3 to 4 MiB), but the gate as measured still
failed: 191 MiB after events against 167 MiB steady (+14.4%). ADR 0041 closed
it by compacting after large deletions and was withdrawn for its cost (bulk
deletion visibility lag 181 to 3052 ms, two full rebuilds, a 371 MiB peak).

An isolated rerun on the same 213k mirror split the gap with glibc's
`malloc_info`: about 7 MiB is search scratch first touched by the probe
queries (the harness read "steady" before any query ran), about 15 MiB is live
heap held by the 22k deleted entries still in the base engine until its next
rebuild, and about 3 MiB is free heap a trim returns. Against a warm baseline
the gap was +10.7%: almost entirely deleted-entry retention, which the user
chose to keep when withdrawing ADR 0041.

## Decision
The gate measures what it was meant to catch, against a **warm steady
state** (RSS after startup and the benchmark's 900 corpus queries, the same
kinds the event probes run):

- **No retained free heap.** After burst, rename, delete and churn settle, a
  diagnostic `malloc_trim(0)` returns at most 5 MiB (`retained_free_mib`).
- **Nothing grows but the catalog.** The harness then grows the catalog
  until the live delta passes its bound and the base engine is rebuilt
  without the deleted entries, repeats the warm-up, and requires RSS within
  10% of the warm steady RSS (`rebuilt_growth_pct`). The entries added to force
  the rebuild (about 3.5% more at 213k) stay in the catalog, which only makes
  the check stricter.
- **Deleted-entry retention is a documented, bounded limit.** RSS right after
  the events is still reported against both baselines (`growth_pct`,
  `growth_vs_cold_pct`). Retained entries never exceed the catalog's size at
  the last rebuild, and go at the next one (live-delta compaction or
  restart).

## Alternatives considered
- **Keep the old gate and compact deletions immediately** (ADR 0041):
  withdrawn for its bulk-deletion cost.
- **Compact deletions later, when the writer is idle.** No visibility lag, but
  still a full rebuild (about 2 CPU-s and a transient peak of about twice
  steady RSS) to return about 10 MiB, which the user had already judged not
  worth it.
- **Relax the threshold to 15%.** Hides growth from any cause, including a
  real leak, instead of separating retention from leaks.

## Consequences
- The gate fails on retained free heap or on growth that survives a rebuild,
  the two things that would make RSS climb over days of uptime.
- A daemon that deleted a large tree keeps that tree's base-engine memory
  until its next rebuild. Measured values are in the
  [M7 plan](../m7-plan.md#real-desktop-review-2026-10-11).
