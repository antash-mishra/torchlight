# 0004. Lexical module split, byte paths, two-phase responses, resident vectors

- **Status:** Accepted, amended by [0005](0005-cache-and-response-correctness.md)
  (typo gating, path fields, vector paging)
- **Date:** 2026-10-02
- **Refines:** 0003

## Context
Review of the 0003 plan found:
- The `trigram` module had grown to cover four retrieval channels, so its name
  no longer matched its job.
- Character posting lists barely narrow queries made of common letters.
- An unconditional edit-distance scan over 500k basenames per keystroke
  threatens the lexical latency target.
- Query embedding can consume most of the hybrid budget.
- Linux paths are bytes, but JSON requires valid UTF-8.
- About 216MB of resident vectors is heavy, and mmap would violate the
  no-I/O query path rule.

## Decision
- **Module split:** `lexical` orchestrates the separate `prefix`, `trigram`,
  `subseq` and `typo` channel modules.
- **Subsequence:** use a per-path 64-bit character mask scan plus incremental
  narrowing. Character posting lists remain a benchmark alternative.
- **Typo:** use a deletion-neighbourhood index, gated to run only when other
  channels return too few strong candidates. A gated Myers scan remains the
  benchmark alternative.
- **Two-phase responses:** send the lexical results immediately, then a fused
  `final` response for the same request id. Targets: lexical p95 < 5ms, final
  p95 < 10ms.
- **Byte paths:** store `path`/`name` as BLOBs. IPC sends a UTF-8 `display`
  string plus `path_b64` for non-UTF-8 paths. Actions always use exact bytes.
- **Vectors:** keep them resident with no mmap, under a configurable budget
  (default 150MB at 500k paths). Reduce dimensions first, measured against the
  float reference.
- **Naming:** use `catalog_gen` and `emb_gen`, never a bare "generation".
- **Crawling:** skip hidden directories by default, with an allowlist.

## Alternatives considered
- Keeping one `trigram` module: violates single responsibility and naming.
- Mmapped vectors: lower RSS, but page faults on the query path.
- Single-phase hybrid responses: typing latency bounded by inference speed.
- Lossy UTF-8 paths only: some files could not be opened.

## Consequences
- More modules, each small and independently testable.
- Clients must handle two responses per request and byte paths.
- The model pipeline must include dimension reduction to fit the budget.
