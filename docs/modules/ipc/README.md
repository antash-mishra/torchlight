# ipc

> **Status:** Planned
> **Source:** `src/ipc/ipc.c` · **Header:** `include/torchlight/ipc.h`
> **Tests:** `tests/unit/test_ipc.c`

## Purpose
Unix domain socket protocol between the daemon and CLI/GTK clients; TUI optional.

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
- Socket: `$XDG_RUNTIME_DIR/torchlight.sock`.
- Versioned JSON-line requests and responses; input/output sizes are bounded.
- Query requests carry request ids; responses carry request/search ids,
  `catalog_gen`, indexing status, a `phase` (`lexical` or `final`), and results.
- Assign search ids before first response and enqueue history once per query.
  Both phases use the same pinned `catalog_gen`/`emb_gen` pair. Request ids are
  scoped to a connection; reject duplicate active ids.
- **Two-phase responses:** lexical results are sent immediately; when semantic
  search is enabled, a `final` response with fused results follows for the same
  request id. Lexical-only mode sends one `final` response.
- Disabled/unavailable/failed/timed-out semantics returns a terminal lexical
  fallback with status/reason. Every active uncancelled request must finish.
- **Paths:** each result has `display` (valid UTF-8, invalid bytes replaced with
  U+FFFD). Also carry exact unnormalized `path` for UTF-8 paths or `path_b64`
  otherwise; display is never an action path. File ids use decimal strings to
  preserve 64-bit precision. Reject decoded paths containing NUL.
- Resolve requests validate ids against the current catalog before launching;
  return current paths or stale-result errors. Open-recording requests carry
  file/search ids and a unique launch-event id; recording is asynchronous and
  retries are deduplicated.
- Escape queries/paths, including newlines. Cancel queued obsolete queries and
  let clients suppress stale responses while typing.
- Discard queued obsolete work; ignore already-running obsolete inference, since
  backend preemption is not assumed. Cancellation is per client/request.
- Nonblocking I/O, bounded output queues and timeouts prevent slow clients from
  blocking queries or retaining pinned snapshots indefinitely.
- Socket access is restricted to the current user.

## Data flow
_TODO after implementation._

## Invariants
_TODO after implementation._

## Performance
_TODO: complexity, memory use, `make bench` numbers with date._

## Testing
_TODO after implementation._

## Gotchas
_TODO after implementation._

## Related
- [daemon](../daemon/README.md)
- [cli](../cli/README.md)
- [ui](../ui/README.md)
