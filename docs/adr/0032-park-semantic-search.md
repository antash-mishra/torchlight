# 0032. Park semantic search; M5 follows M6

- **Status:** Accepted
- **Date:** 2026-10-08

## Context

Semantic search (M4) is implemented and opt-in through `torchlightd --model`.
Its remaining acceptance gates are final-phase p95 below 10 ms at 500k paths
and a broader model and relevance comparison. ADR 0031 cut the 500k final
phase from 95 to 24 ms p95, but meeting 10 ms would need further concurrency
work (semantic search alongside the lexical search, a threaded first pass).

Measured value is limited. The large relevance gain (held-out nDCG@10 0.500 to
0.969) comes from a 36-entry fixture built to test concepts that name matching
cannot express. The real Documents check confirmed only that exact names keep
the first result; it did not show semantic search finding what lexical search
missed. Only names and nearby folders are embedded, never contents. The costs
are about twice the resident memory at 500k (603 versus about 280 MB), several
minutes of first embedding, and the most complex part of the daemon. Lexical
search already meets its own 5 ms typing target.

## Decision

Park semantic search:

- Keep all semantic code, tests and the opt-in `--model` flag; the default
  daemon stays lexical-only and runs none of it.
- Keep ADR 0031's prefix-shortlist search as the opt-in path's vector search.
- Defer the M4 acceptance gates (final-phase latency at 500k, broader model
  and relevance comparison). Do not start the overlap or threading work.
- M5 (personal recommendations) follows M6 directly. Its comparison uses
  lexical ranking, plus hybrid ranking where a model is enabled.

Revisit when real use shows queries that only semantic search answers, or when
document contents are indexed.

## Alternatives considered

- **Finish the M4 gates.** More concurrency in the daemon for a feature with
  unproven real-world value.
- **Delete semantic search.** Simpler code, but removal touches the daemon,
  IPC phases, schema tables and tests, and discards a working opt-in feature
  that costs nothing when disabled.

## Consequences

- Semantic search remains supported but unaccepted at large catalogs; its
  measured limits stay documented in the M4 reports.
- Changes to shared code must keep `make test` passing, including the
  semantic daemon tests, so the parked path does not rot.
- Milestone order is M6, then M5; M4 acceptance resumes only by a later
  decision.
