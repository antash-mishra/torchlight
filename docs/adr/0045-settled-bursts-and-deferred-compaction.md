# 0045. Settled event passes and deferred compaction

- **Status:** Accepted
- **Date:** 2026-10-12
- **Amends:** [0030](0030-m6-incremental-indexing.md) (compaction runs as soon as
  the live delta passes its bound)

## Context
After M7 Phase 2 a burst of 20,000 new files in 2,000 new folders became
searchable about 3 s after it was written, against about 1.7 s in some runs of
the build before Phase 2 ([health report](../reports/m7-phase2-health-report.html)).
Every run on the 213k mirror showed the same pattern: three scoped passes, one
delta and two full rebuilds.

- The first pass starts 100 ms after the burst's first event. Until a scan
  watches the new folders, the burst sends no further events, so the pass
  catches whatever exists by then (a few thousand entries).
- Phase 2's crawl finishes that pass quickly, while the burst is still running.
  The rest arrives as events for the newly watched folders, and the next pass
  runs 100 ms after the first of them, again in the middle of the burst.
- The delta bound is max(4096, base/32): 6.6k entries at 213k. The burst holds
  22k, so at least one pass exceeds it, and each pass that does rebuilds the
  whole engine inline (about 1.3 s) before it publishes.

Renaming the burst's tree touches 22k rows at once and paid the same inline
rebuild: 2.1 s until the new paths were searchable.

## Decision

**Settle event passes.** A filesystem event schedules the next pass 100 ms
after the latest event, never more than 1 s after the first event since the
last pass began (`WRITER_SETTLE_MS`). A single change keeps its 100 ms delay;
a burst is rescanned once it pauses, or once a second while it lasts. Requests
keep the earliest time asked for: `reconcile`, overflow, the repair and
backstop timers, mounts and replaced-directory rescans. A failed pass retries
after its own delay. Settling stops once 512 scopes are pending
(`WRITER_SETTLE_SCOPES`, half the 1,024 a scoped pass holds), because past
that a pass scans every root. A recursive deletion adds a scope per removed
directory, and the first version of this change turned the 22k-entry
deletion into a 1.2 s full scan; with the limit it stays a few scoped passes.

**Defer compaction.** A delta may grow to four times its bound
(`tl_delta_limits.slack`, `WRITER_DELTA_SLACK`). A pass whose delta passes
the bound publishes it at once and marks the compaction pending
(`delta_oversized`). The full rebuild runs between passes:
- as soon as no pass is pending; events that arrived since the last drain are
  drained first and go before it;
- otherwise 2 s after the compaction became due (`WRITER_COMPACT_DEFER_MS`),
  even while events keep coming.

Beyond four times the bound a pass still rebuilds inline. Search keeps the
published delta until the compaction publishes, so a failed compaction only
retries (100 ms while a reader holds the previous view, else 1 s).

**Republish the same generation.** The compaction rebuilds the committed
catalog the delta already shows, at the same `catalog_gen`. `catalog_publish`
still refuses an equal generation, which guards against a stale or duplicate
publication. The compaction uses `catalog_publish_compacted`, which also
accepts the active generation and refuses older ones. A `catalog_gen` names
one committed catalog, so base plus delta and the rebuilt base answer every
query alike (a unit test compares them). The semantic worker compares
generations for equality and sees no change, which is correct.

`status.indexing` reports `compaction_pending` and `deferred_compactions`.
`active` is true while a compaction runs, so benchmarks and tests that wait
for an idle daemon also wait for it.

## Alternatives considered
- **Raise the delta bound** (for example to base/8). The burst would publish
  as a delta, but the large delta would stay until a later change pushed it
  past the bound. Every small change would rebuild the merged delta (about
  0.2 s instead of milliseconds), and every query would search it, for as
  long as it lasted. Deferral keeps the large delta for about one rebuild's
  time.
- **Settle only, keep inline compaction.** One pass then catches most of the
  burst, but it is past the bound, so the burst becomes searchable only after
  a full rebuild (estimated about 2 s; not measured). Settling saves passes;
  deferral is what makes the burst searchable quickly.
- **Defer compaction only.** The burst would publish quickly too, but every
  mid-burst pass (about one per 100 ms plus its scan) would rebuild a growing
  merged delta. Settling caps that at two passes for this burst.
- **Wait for a quiet period before compacting.** The writer is idle as soon
  as no pass is pending. Waiting longer would keep the large delta (slower
  small changes and queries) without saving work.
- **Advance `catalog_gen` for the compaction.** A compaction would then need a
  write transaction on the persistence thread to bump the stored generation,
  and an equal-content view would look like new data to every consumer.

## Consequences
- 213k mirror, `-O3`, medians: the 20k burst is searchable 0.5 s after it is
  written (was 3.0 s) with one rebuild instead of two (2.0 CPU-s instead of
  3.6). Renaming the 22k tree takes 0.47 s (was 2.1 s), and deleting it 2 to
  150 ms in scoped passes. Single changes are unchanged (about 107 ms).
- For about one rebuild's time after a large change (about 1.6 s at 213k),
  search covers a larger delta while the rebuild runs on another core: query
  p50 0.91 ms and p95 2.7 ms, against 0.77 and 1.85 ms idle. Peak RSS during a
  rename is a few MiB higher than an inline rebuild, because the large delta
  and the new base exist together.
- A change that arrives while a compaction runs waits for it, at most one
  rebuild. Before, the same rebuild ran before the large change itself became
  searchable.
- Under a continuous event stream a pass runs at least once a second, and a
  new name can take up to 1 s to appear instead of about 0.1 to 0.2 s.
- Tests: the settle window and its cap (`bursts_settle`), deferred compaction
  after the bound (`delta_compaction`), slack and `delta_oversized`
  (`test_delta.c`), and compacted publication (`test_catalog.c`).
