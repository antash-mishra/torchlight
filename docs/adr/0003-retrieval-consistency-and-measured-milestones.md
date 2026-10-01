# 0003. Complete lexical retrieval, consistent generations, and measured semantic choices

- **Status:** Accepted
- **Date:** 2026-10-02
- **Supersedes:** 0001 and 0002

## Context

Review exposed missing abbreviation/short-query candidates, conflation of
subsequence scoring with typo correction, unsupported performance claims, and
incomplete memory/concurrency/recovery plans. Choosing an embedding model before
evaluating filenames and postponing the popup delayed a usable launcher.

## Decision

- Combine exact/prefix, relaxed trigram, and character/subsequence retrieval.
  Handle short queries and bounded edit-distance fallback explicitly. Measure
  candidate recall before limiting candidates.
- Treat lexical p95 <5ms and hybrid p95 <10ms at 500k paths as measured targets
  on a documented machine. Include embedding, client round trips, indexing load,
  total/peak RSS, and ranking quality in evaluation.
- Keep models/backends swappable and choose in M4 using real filename queries,
  C inference parity, and a float cosine reference. Binary/int8 with a shortlist
  of 200 is an experiment; account for int8 memory and quantization metadata.
- Serialize catalog writes, commit before publishing immutable generations, and
  reclaim after readers release them. Make history writes asynchronous and
  reconcile missed events, unavailable watches, restarts, and failed scan scopes.
- Build lexical CLI → daemon/watch → GTK4 popup/service → semantics →
  personalization. TUI remains optional.

## Alternatives considered

- Trigram-only retrieval: misses abbreviations without contiguous overlap.
- Subsequence-only scoring: cannot handle arbitrary inserted query characters or
  substitutions; requires a separate edit-distance path.
- SQLite FTS5 as the resident engine: useful baseline, but queries use SQLite and
  trigram full-text search misses strings shorter than three characters.
- Immediate default model/ANN choice: defers evidence until after integration.
- In-place mutation with long reader locks: risks inconsistent indexes or
  query stalls while background work runs.

## Consequences

More candidate channels and lifecycle bookkeeping require explicit benchmarks
and recovery checks. Generation staging increases peak memory and must be
budgeted. The desktop launcher becomes usable before semantic integration,
while model quality and speed remain decisions based on measurements.
