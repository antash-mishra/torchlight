# M1/M2 readiness for M3

**Status: ready to start M3, verified 3 October 2026.** The user accepts roughly
6 ms p95 lexical latency and defers further optimization until the whole system
is built. The original 5 ms target remains a recorded optimization goal.

## Completed scope

| Milestone | Implemented and verified |
|---|---|
| M1 | C17 build/checks; byte-safe crawler and SQLite catalog; configuration, roots and allowlists; Unicode normalization; prefix/initials, subsequence, trigram and bounded typo search; directory context; deterministic ranking; CLI; synthetic and real-corpus benchmark support. |
| M2 | Resident saved-catalog startup; bounded Unix-socket IPC; immutable snapshot leases; background reconciliation and inotify updates; atomic publication/recovery; byte-safe file-id resolution; status; optional asynchronous/deduplicated history; singleton locking and clean/crash restart. |

The previous [M2 review](m2-review.md) fixes remain covered: replacements retire
stale file/directory ids and descendants, and scans continue when inotify setup
fails. Migration, rollback, offline roots and watcher recovery remain tested.

This change fixes two additional query-contract issues:

- Trigram deduplication now uses in-place integer heapsort. Accepted queries
  of 131 bytes or more previously caused glibc `qsort` to allocate scratch.
  Allocator interposition now verifies zero query allocations for long queries
  and a 100k-entry workspace with active workers.
- Typo lookup now enforces the documented inclusive 3–32-symbol query range.
  Two-symbol fragments use prefix/subsequence matching without an incorrect
  complete-token typo boost; out-of-range long words also skip typo lookup.

Complete basename bitmap filtering, parent-entry unions, bounded word evidence
caches, exact score bounds and prestarted scoring workers reduce query work.
They introduce no candidate budget. Regression tests cover exact multiword
names, cache eviction, capacity-independent results and analytic large-corpus
scores. See [ADR 0013](adr/0013-query-contracts-and-resident-filters.md).

## Verification and measured limits

`make`, `make test` and `make lint` pass with zero warnings. Tests include
ASan/UBSan with leak detection, allocator interposition, CLI integration and
daemon integration. `make bench` and `make bench-daemon` complete successfully.
`AGENTS.md` and `CLAUDE.md` remain identical. No dependency was added.

A development differential check against the prior query algorithm, with the
same typo-bound correction, matched ordered ids and scores for 6,026
typing/backspace cases at 50k paths and 6,082 at 500k paths. Both ten-result and
1,000-result queries are included. The regular held-out benchmark uses seed 2;
optimization used the separate seed-1 tuning workload.

| Measurement | 50k paths | 500k paths |
|---|---:|---:|
| Typing engine p95 | 0.518 ms | 5.970 ms |
| Independent whole-query engine p95 | 0.701 ms | 6.154 ms |
| Leased whole-query engine p95 | 0.676 ms | 6.013 ms |
| Daemon engine p95 | 0.858 ms | 5.615 ms |
| Socket round-trip p95 | 0.964 ms | 5.706 ms |
| Round-trip p95 during rebuilding | 0.966 ms | 6.776 ms |

Daemon catalogs deduplicate to 49,569 and 494,362 entries. These are warm,
unpinned observations on the documented shared reference machine, not worst-case
guarantees. All nine synthetic fixtures pass at both sizes. Held-out Recall@10
remains 0.615 / 0.531 and candidate Recall@1000 remains 0.920 / 0.825.
Full conditions, p50/p99, memory and raw output are in
[evaluation](evaluation.md#2026-10-03-m1m2-readiness-adr-0013).

Further work is deferred: reaching the original 5 ms target; reducing complete
engine rebuild cost (about 4.06 seconds and 642.5 MiB peak RSS for a 101-file
update near 500k paths); and improving unfinished misspelled-token relevance.
The update benchmark uses saved synthetic paths plus a small physical root,
not a 500k-file crawl. These limits do not block starting M3 under the accepted
scope. ThreadSanitizer was attempted but could not start on this host because
of its runtime memory-mapping error; no ThreadSanitizer pass is claimed.

## GUI design to follow

Implement [the M3 GTK4 specification](m3-gui-design.md) and use the
[interactive preview](m3-gui-preview.html) as the layout reference:

- A 680-logical-pixel floating popup with native GTK theme and symbolic icons.
- One focused search entry, eight visible basename/parent rows, and a compact
  status/keyboard footer.
- Hotkey toggle, arrow selection, Enter to open, Ctrl+Enter to reveal and Escape
  to dismiss; current-id resolve before each action.
- Responsive asynchronous IPC, obsolete-response suppression, stable selection
  and explicit indexing, empty, unavailable-service and stale-action states.

GTK code, GTK4 dependency setup, the systemd user unit and desktop integration
belong to M3. Validate focus, activation and placement first in Cinnamon X11,
then record Wayland behavior separately. The preview's JavaScript syntax is
checked; actual GTK rendering and desktop behavior are M3 acceptance work.
