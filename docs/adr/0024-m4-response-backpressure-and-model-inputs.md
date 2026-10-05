# 0024. M4 response backpressure and model inputs

- **Status:** Accepted
- **Date:** 2026-10-06

## Context

Review found that a lexical frame and a fast semantic final could each fit the
1 MiB response limit while overflowing their shared client queue together.
`semantic_take` consumed completed jobs even when the caller buffer was too
small. Semantic finals also omitted indexing/history status, clearing the
popup's degraded warning. Finally, opening a FIFO as a model blocked the
semantic worker and prevented shutdown from joining it.

## Decision

Drain earlier socket output before taking a normal semantic final. Preserve
the semantic token and snapshot on `TL_LIMIT`, including cancellation/deadline
encoding, so the caller can retry or cancel without losing the response.
Keep the existing 1 MiB queue and five-second client deadline.

Reserve 1 KiB within the worker's response budget. The coordinator appends
current indexing/history/semantic status to every terminal semantic envelope,
using the same formatter as lexical responses. Snapshot ids still describe the
query's coherent retrieval view; status describes current service health.

Open model paths with nonblocking flags and require a regular file using
`fstat` on the opened descriptor before reading. Symlinks to regular files are
valid; FIFOs, directories and devices fail with `TL_IO`. Checking the actual
descriptor avoids a validation/open race. No new dependencies are needed.

## Alternatives considered

Doubling each client buffer increases memory without establishing correct
backpressure for multiple responses. Keeping old indexing status in the popup
would hide incomplete final envelopes from other clients. Checking a model
path with `stat` before a blocking open leaves a replacement race.

## Consequences

Individually valid large phases complete under the existing memory bound.
Pending jobs retain snapshots while earlier output drains, with cancellation
and client expiry releasing them. Final results preserve degraded warnings and
carry current background status. Semantic response payloads have 1 KiB less
space, keeping the fully annotated envelope within the original frame limit.
Stream model inputs fail promptly and lexical fallback remains available.
Regression coverage includes ready/cancelled buffer retries, fast/slow readers,
CLI final consumption, offline-root status and FIFO shutdown.
