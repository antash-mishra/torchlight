# 0041. Count retired base entries toward compaction

- **Status:** Accepted; implemented and verified 2026-10-11
- **Date:** 2026-10-10
- **Refines:** 0030 (incremental indexing), 0039 (returning freed memory)

## Context

The M7 Phase 1 benchmark records 192 MiB RSS after burst, rename, deletion
and churn, versus 168 MiB steady: +14.3%, above the 10% acceptance limit.
Trimming releases only 3 MiB. A full rebuild during the rename includes the
temporary tree in its base, and subsequent deletion hides those entries
with tombstones. They remain allocated because the delta compaction bound
counts only live delta entries. Deletion-only workloads can retain a large
obsolete base indefinitely.

The [Phase 1 review fixes](../m7-plan.md#phase-1-review-fixes)
close this gate before Phase 2. Number 0040 is reserved by the
existing Phase 2 plan.

## Decision

Keep the existing threshold, max(`min_entries`, base entries /
`divisor`), with writer defaults 4096 and 32, and compacting when the sum of
the following exceeds it:

- Unique base entries retired through the current batch, including both
  deletions and entries superseded by the delta.
- Live pending delta entries whose ids do not exist in the base.

This counts a changed base id once, includes deletions immediately, and
stops counting entries created and removed entirely within the delta.
Calculate the projected count before constructing the delta engine through
a catalog helper that preserves snapshot encapsulation. Keep the existing
whole-snapshot limits and error propagation.

Use the current `TL_LIMIT` path to rebuild from the committed catalog and
publish on the indexing thread. Reset bookkeeping only on successful
publication. Existing leases protect old snapshots; reclamation and the
throttled allocator release hook return memory after the final reference.
This policy introduces no new timer, dependency or filesystem scan.

## Alternatives considered

- **Trim more often.** Trimming cannot free live base entries; the recorded
  manual trim recovers only 3 MiB.
- **Use live delta entries plus all tombstones.** This double-counts changed
  base entries present in the delta and triggers unnecessary rebuilds after
  updates and renames.
- **Check only the source snapshot's tombstones.** This misses deletions in
  the current batch and may never compact if no later change arrives.
- **Defer until the next periodic scan.** A no-change full scan need not
  rebuild the engine, and the hourly backstop may be disabled.
- **Relax the RSS target.** This leaves the retained-base problem unresolved.
  The fix preserves the original acceptance gate.

## Consequences

Deletion-heavy workloads rebuild more often and have higher update
CPU and latency at threshold crossings. Small updates continue to publish
deltas. Query algorithms, immutable snapshot ownership and commit-before-
publication remain unchanged.

The matched 213k rerun finishes at 181 MiB RSS versus 167 MiB steady (+8.4%),
within the original 10% gate before manual trim. The reviewed binary finishes
at 190 versus 166 MiB (+14.5%). Deleting the temporary 20k tree crosses the
bound in two batches: two full builds cost 3.40 CPU-s and 3052 ms visibility
lag, against no full builds, 0.38 CPU-s and 181 ms previously. The deletion
peaks at 371 MiB; the run's largest event peak is 377 MiB during the rename.
The 500k scale check needs one deletion rebuild, costs 4.00 CPU-s and ends
at +2.2% RSS. These are whole-scenario costs, not per-build estimates.

Catalog/delta tests cover input validation, exact boundaries, batched and
accumulated deletions, mixed changes, repeated updates, abandonment and old
reader survival. The writer test injects a failed compaction publication,
recovers from committed storage, and verifies the release hook waits for an
old snapshot pin. Tests and lint pass. See the
[review measurements](../m7-plan.md#phase-1-review-results-2026-10-11) for
conditions, query latency, raw results and the remaining Phase 2 scope.
