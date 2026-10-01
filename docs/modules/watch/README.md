# watch

> **Status:** Planned
> **Source:** `src/fs/watch.c` · **Header:** `include/torchlight/watch.h`
> **Tests:** `tests/unit/test_watch.c`

## Purpose
Keeps the index live using inotify: creates, deletes, renames.

## Responsibilities
_TODO after implementation._

## Public API
_TODO after implementation: functions and pointer ownership rules._

## Design
- Watches each indexed directory.
- Coalesces bursts of events before applying.
- Handles watch-limit exhaustion gracefully (falls back to periodic rescan).
- Registers watches during crawling, then reconciles affected directories.
- Detects queue overflow and schedules reconciliation; also rescans on restart
  and periodically to repair missed events.
- Pairs rename cookies where possible; directory moves update descendant paths.
- Unreadable/offline scopes do not prove deletion. Remove missing entries only
  after a successful scan of their scope.
- Submits changes to the writer; never mutates a `catalog_gen` pinned by a query.
- Queue saturation schedules reconciliation instead of silently losing updates.

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
- [crawl](../crawl/README.md)
- [store](../store/README.md)
