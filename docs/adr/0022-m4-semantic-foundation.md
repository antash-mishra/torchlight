# 0022. Begin M4 with swappable embeddings, float reference and explicit fusion

- **Status:** Accepted
- **Date:** 2026-10-05
- **Refines:** 0003, 0005 and 0019

## Context

M3 Part 2 supplies the lexical quality baseline. M4 needs independently testable
embedding/vector/ranking modules before trained-model parity, compression and
daemon execution can be evaluated. Their module docs previously contained only
plans. The user selected English-first model evaluation and asked whether the
semantic path should use an autoregressive or retrieval embedding model.

## Decision

Implement a local embedding adapter, exhaustive float cosine reference and RRF
baseline first. Use embeddings for semantic retrieval alongside existing name
search. Autoregressive query rewriting or generated answers are separate future
experiments and are not needed for this launcher milestone.

The adapter copies immutable full `emb_gen` metadata, owns backend context only
after successful creation, distinguishes query/document roles, and validates
normalized finite output. It accepts prepared UTF-8; raw paths keep their exact
byte identity in lexical search/actions. Versioned text preparation and trained
backends remain pending. No backend/model is chosen from published scores.

Vector builders reserve owned memory within an explicit byte budget, enforce one
`emb_gen`, copy normalized rows and seal for immutable queries. Workspaces are
created before inference-independent scanning. Float cosine uses double
accumulation; top-k ordering is deterministic. RRF uses preallocated scratch,
one vote per source, explicit exact-path/basename tiers and verbatim lexical
fallback. Reject conflicting paths for matching source ids. Generic in-place
sorting belongs to core; the existing optimized integer sort stays unchanged.

Benchmark the reference at 50k/500k rows before deciding compressed retrieval.
The first 256-dimensional run reserves 516 MB at 500k and scans at about
161 ms p95, excluding inference and lexical/IPC work. It is an evaluation
reference, not a production format satisfying M4's target. Record these limits
and gate subsequent binary/int8 or ANN choices on recall, relevance and latency.

## Alternatives considered

- Selecting the model/runtime before labeled launcher measurements: cannot
  establish filename/application relevance or meet the local latency budget.
- Implementing binary/int8 first: lacks an independent full-float reference for
  detecting recall or scoring loss.
- Using RRF without exact-match tiers: semantic overlap can outrank an exact
  user target.
- Shipping fake embeddings in the daemon: exercises plumbing without useful
  semantics. Fixture callbacks remain unit-test-only.

## Consequences

M4 is started, not complete. The daemon still serves lexical terminal responses.
Next work is English model/representation comparison, C parity, evaluated
compression, persisted/staged embeddings, coherent file/desktop leases and
bounded two-phase execution with cancellation/deadline/error fallback. These
modules do not themselves publish snapshots or verify stale metadata revisions.
No third-party dependency was added; numerical helpers use the platform C math
library. Model/tokenizer runtime additions must be discussed before adoption.

See [implementation and measurements](../m4-implementation.md).
