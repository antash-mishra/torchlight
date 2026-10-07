# 0026. Search workers, SIMD and incremental indexing

- **Status:** Accepted; implementation planned
- **Date:** 2026-10-07

## Context

M1/M2/M3 and M3 Part 2 are implemented. M4's opt-in native Potion, versioned
cache and two-phase hybrid search are functional, while large-catalog latency
and broader relevance acceptance remain open. M5 personalization is planned.

GTK already uses asynchronous IPC and worker-based launch actions. Lexical
search still runs in the daemon poll loop, and a shared background worker owns
filesystem reconciliation, full engine construction and history persistence.
The recorded native lexical run has 9.622 ms whole-query p95 at 500k paths;
historical full-rebuild measurements show substantial update lag and peak RSS.

The user selected Frizbee and requested worker separation first, SIMD search
second and incremental indexing third, without adding many milestones.

## Decision

Append **M6: search responsiveness and indexing performance** as one milestone
with three ordered implementation steps. Preserve existing milestone ids. M6
executes next, before M5; outstanding M4 acceptance work remains tracked and
does not block starting M6. This updates the execution order in ADR 0019.

1. **Worker separation.** Keep the GTK process separate from the daemon.
   Separate the daemon IPC loop, search, indexing and history/persistence work.
   Keep only the newest pending query per client, check cancellation between
   batches and suppress stale completions. Preserve coherent file/application
   leases, semantic deadlines, bounded output and safe shutdown. Indexing builds
   changes privately; the persistence owner serializes catalog/history commits,
   with filesystem scans and index construction outside write transactions.
   Preserve existing semantic-cache retry/coherence contracts.
2. **Frizbee SIMD search.** Use Frizbee through its C ABI as the production fuzzy
   matcher. The dependency is selected and authorized by the user; pin its source
   and build tooling during integration. There is no library-selection or
   comparative matcher evaluation phase. Preserve prefix/subsequence/trigram/typo
   candidate coverage, exact priority, field weights, normalized Unicode and raw
   path bytes. Adapt the wrapper's buffers, score bounds and cancellation to
   maintain query contracts. Reuse one bounded scoring pool where necessary.
3. **Incremental indexing.** Apply coalesced changes to affected entries and
   directories, share unchanged immutable blocks and compact in the background.
   Routine small updates should not scan all roots or rebuild the whole engine.
   Retain commit-before-publication, incarnation/rename identity, semantic
   invalidation, successful-scope deletion, overflow reconciliation and recovery.
   Full rebuilds remain available for recovery and compaction.

Resident indexes remain the baseline. A read-only mmap file format is not a
prerequisite. Personalization remains M5; content extraction is outside M6.

## Alternatives considered

- Three new milestones would fragment one requested performance sequence.
- A matcher-library selection phase is unnecessary because the user selected
  Frizbee. Correctness, relevance and performance validation still apply to the
  integrated implementation.
- Trigram-only retrieval or a fixed pre-scoring candidate cap could lose short
  queries, abbreviations and useful typo matches.
- Separate catalog and history writers to the same SQLite database would still
  compete for its write lock; expensive preparation should leave the persistence
  worker available for short serialized writes.
- A mandatory mmap conversion adds format/publication work before the requested
  worker and search improvements, without established warm-query benefit.

## Refinement (2026-10-07)

A gprof profile of `bench_lexical` at 500k attributes about 46% of query time
to scoring (`fuzzy_score`, `entry_score`, the result heap) and the rest to
retrieval fan-out: roughly 18k prefix-hit callbacks and 41k directory-ancestor
visits per query. A faster scorer alone therefore caps near 1.4x, so step 2
also bounds candidate volume. Frizbee's C ABI (`bindings/frizbee-c`, from
v0.13.0) requires UTF-8 input without normalization and allocates its result
list; the wrapper keeps `tokenize` as the normalizer, keeps the C scorer for
non-UTF-8 paths, and re-derives score bounds. Step 3 splits into scoped
reconcile, a base-plus-delta segmented engine with background compaction, and
incremental semantic snapshots, because the semantic worker currently recopies
every path on each publish. Details and baseline numbers are in the
[M6 working plan](../m6-plan.md).

## Consequences

The roadmap gains one milestone, executed as worker separation → Frizbee SIMD
search → incremental indexing. Existing functionality is not marked complete
merely because it is planned. M4 acceptance and M5 remain distinct.

Each step retains sanitizer/lint, byte-path, exact-match, field/typo relevance,
snapshot lifecycle, cancellation and crash/recovery coverage. Queries must remain
allocation-free and avoid SQL/filesystem waits. Measure 50k/500k engine and IPC
p50/p95/p99, indexing-load latency, update lag, history counters and steady/peak
RSS. Target warm lexical p95 below 5 ms and lower small-update lag/peak RSS against
the recorded full-rebuild baseline. No speedup is claimed before implementation.

See [PLAN.md](../../PLAN.md), [milestone status](../milestone-status.md) and
[architecture](../architecture.md).
