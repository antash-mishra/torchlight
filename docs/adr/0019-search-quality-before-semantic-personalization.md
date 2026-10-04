# 0019. Search quality before semantics and personalization

- **Status:** Accepted; milestone implementation planned
- **Date:** 2026-10-05

## Context

M1/M2/M3 supply a working resident desktop launcher. Feature completion and
synthetic known-item benchmarks do not establish broad relevance. Application
names can be buried by competing filenames, and unfinished misspelled words
can fail retrieval altogether. ADR 0018 fixes application-name weighting and
tie ordering, but that does not resolve the wider quality gaps.

## Decision

Insert **M3 Part 2: search quality and relevance** before the existing M4/M5
milestones, preserving their identifiers. Part 2 adds realistic labeled mixed
queries, explicit field-aware name/context ranking, indexed prefix typo matching
and a comparison of optimal versus greedy fuzzy scoring. It must preserve exact
priorities, complete candidate semantics, query-capacity consistency, byte-safe
paths and allocation-free lexical queries. Report held-out relevance by query
class plus latency and memory at 50k/500k paths.

**M4: hybrid semantic search** follows that stronger baseline. Evaluate small
local embedding models on the same relevant task, build vectors in the
background, and compare vector/fused retrieval against improved lexical search.
Begin with float cosine correctness and RRF fusion, comparing quantization and
alternative fusion rules before choosing them. Preserve immediate lexical
responses, coherent metadata/embedding snapshots, bounded cancellation and
terminal fallback.

**M5: personal recommendations and ranking** then uses optional file/application
open history, frecency and query-to-open summaries to promote useful results.
Bound boosts, retain clear-history/retention/disabled-history behavior, and
verify that personal preferences do not bury exact names or strong name evidence.

## Alternatives considered

- Adding embeddings before repairing name retrieval would leave basic typing
  failures dependent on an optional semantic path.
- Personalization before relevance evaluation could reinforce noisy results
  rather than demonstrate useful search.
- Declaring a model or fusion rule best from public benchmarks would substitute
  another workload for the launcher's actual queries and hardware.

## Consequences

The sequence is M3 Part 2 → M4 → M5. Only the existing name-weighting/tie fixes
are implemented within Part 2; the rest is planned. Model and index choices remain
measured decisions, and new dependencies still require discussion. Physical
fractional/multiple-monitor, Wayland and human screen-reader validation remain
separate platform work. The previously deferred 5 ms target and full-rebuild
optimization are not silently moved into this milestone.

See [PLAN.md](../../PLAN.md), [milestone status](../milestone-status.md) and
[the search quality review](../search-quality.md).
