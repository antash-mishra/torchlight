# Torchlight docs

For a simple visual overview, open [Built and next](system-overview.html) in a
browser. It shows the current system, the planned system, and the next steps.
The implemented M3 popup follows the [GUI specification](m3-gui-design.md) and
[interactive design preview](m3-gui-preview.html), including Cinnamon X11 focus,
keyboard actions, status/error states and asynchronous IPC requirements.

Start here. Read in this order:

1. [`architecture.md`](architecture.md): how the pieces fit together and how a query flows.
2. [`glossary.md`](glossary.md): terms used throughout the code and docs.
3. [`evaluation.md`](evaluation.md): labeled queries, metrics, regression scenarios, recorded results.
4. [`modules/`](modules/): one folder per module, each with a `README.md`.
5. [`adr/`](adr/): Architecture Decision Records, explaining *why* things are the way they are.

## Implementation status

M3 is implemented: installed application/settings search through resident IPC,
GTK4 keyboard popup, safe native launch/open/reveal actions, desktop integration
and a systemd user unit. See [desktop setup](desktop-setup.md),
[M3 verification](m3-completion.md) and [ADR 0015](adr/0015-m3-desktop-catalog-and-launcher.md).
M4 semantic search is next.

M3 proceeded with the user's acceptance of roughly 6 ms p95 at 500k paths.
The original 5 ms target and full-engine rebuild cost remain later optimization
work. The [readiness report](m3-readiness.md) records pre-M3 verification
and measurements. Query-contract fixes and exact resident filtering
are described in [ADR 0013](adr/0013-query-contracts-and-resident-filters.md).

Both findings from the [M2 review](m2-review.md) are fixed and covered by sanitizer
regressions: filesystem incarnation checks retire replacement ids, and inotify
instance failure permits scan-only reconciliation. See
[ADR 0012](adr/0012-filesystem-incarnations-and-watch-fallback.md) for migration
and fallback behavior.

M2's resident [daemon](modules/daemon/README.md) is implemented: bounded Unix
socket clients, lexical-only terminal responses, immutable catalog snapshots,
asynchronous [writer/history](modules/writer/README.md), inotify and reconciliation,
file-id resolution, status, clean shutdown and crash/restart recovery. Integration
and injected-failure tests cover the milestone scenarios. Updates rebuild whole
engines; see [evaluation](evaluation.md) and
[0011](adr/0011-m2-daemon-writer-and-reconciliation.md).

M1's lexical engine and CLI are implemented: prefix, subsequence, trigram and
typo channels with edit scoring, interned parent directories, incremental
narrowing, configuration with allowlists, root deduplication and sync, and a
benchmark with labeled tuning/held-out queries on synthetic and real corpora.
See [evaluation](evaluation.md) for benchmark conditions and recorded results,
the root [README](../README.md) for commands, and
[0008](adr/0008-m1-completion-channels-directories-config.md) for the design.

## Module index

| Layer | Module | Doc |
|---|---|---|
| core | Utilities (vec, hashmap, JSON/base64, byte scopes, sort, masks, worker pool; arena/log planned) | [core](modules/core/README.md) |
| core | Configuration | [config](modules/config/README.md) |
| index | Path tokenizer | [tokenize](modules/tokenize/README.md) |
| index | Lexical candidate orchestrator | [lexical](modules/lexical/README.md) |
| index | Installed applications and settings | [desktop](modules/desktop/README.md) |
| index | Resident catalog snapshots | [catalog](modules/catalog/README.md) |
| index | Exact/prefix channel | [prefix](modules/prefix/README.md) |
| index | Trigram channel | [trigram](modules/trigram/README.md) |
| index | Subsequence channel | [subseq](modules/subseq/README.md) |
| index | Typo channel | [typo](modules/typo/README.md) |
| index | Fuzzy/edit scorer | [fuzzy](modules/fuzzy/README.md) |
| index | Interned parent directories | [dirtree](modules/dirtree/README.md) |
| index | Swappable embedder | [embed](modules/embed/README.md) |
| index | Vector search | [vector](modules/vector/README.md) |
| index | Ranking & fusion | [rank](modules/rank/README.md) |
| storage | SQLite store | [store](modules/store/README.md) |
| fs | Crawler | [crawl](modules/crawl/README.md) |
| fs | inotify watcher | [watch](modules/watch/README.md) |
| ipc | Socket protocol | [ipc](modules/ipc/README.md) |
| service | Resident daemon | [daemon](modules/daemon/README.md) |
| service | Background writer/history | [writer](modules/writer/README.md) |
| bin | CLI client | [cli](modules/cli/README.md) |
| ui | GTK4 popup & optional TUI | [ui](modules/ui/README.md) |

## Current decisions

- [0003: retrieval, consistency, and measured milestones](adr/0003-retrieval-consistency-and-measured-milestones.md)
  supersedes the initial lexical and embedding proposals in 0001/0002.
- [0004: lexical module split, byte paths, two-phase responses, resident vectors](adr/0004-lexical-split-byte-paths-two-phase.md)
  refines 0003.
- [0005: candidate caches, response completion, and snapshot lifetimes](adr/0005-cache-and-response-correctness.md)
  refines 0003/0004; resolves later correctness and memory-accounting gaps.

- [0006: first M1 prefix/subsequence baseline](adr/0006-m1-prefix-subsequence-baseline.md)
  records the initial implementation and remaining M1 gates.
- [0007: partial scans, nested roots and component-scoped parent matching](adr/0007-partial-scans-and-component-parent-matching.md)
  refines 0006 after the first M1 review.
- [0008: M1 completion: channels, directory interning, bounded scans and configuration](adr/0008-m1-completion-channels-directories-config.md)
  completes M1's lexical engine and configuration.
- [0009: unresolved root identities and refresh failures](adr/0009-unresolved-roots-and-refresh-failures.md)
  preserves unavailable aliases and rolls back storage failures during refresh.
- [0010: M2 resident catalog snapshots](adr/0010-m2-resident-catalog-snapshots.md)
  establishes bounded leases, publication, reclamation and coherent catalog loads.
- [0011: M2 daemon, writer and reconciliation](adr/0011-m2-daemon-writer-and-reconciliation.md)
  completes resident IPC, live updates, asynchronous history and recovery.
- [0012: filesystem incarnations and watch fallback](adr/0012-filesystem-incarnations-and-watch-fallback.md)
  retires replacement ids and keeps reconciliation working without inotify.
- [0013: query contracts and complete resident filtering](adr/0013-query-contracts-and-resident-filters.md)
  enforces allocation/typo bounds and specifies exact bitmap/parallel query work.
- [0014: M3 GTK popup design](adr/0014-m3-gtk-popup-design.md)
  fixes the presentation, interaction and desktop acceptance specification.
- [0015: M3 desktop catalog and launcher](adr/0015-m3-desktop-catalog-and-launcher.md)
  records resident desktop discovery, asynchronous UI/actions and schema v3 history.
- [0016: empty popup without default recommendations](adr/0016-empty-popup-without-recommendations.md)
  makes opening/clearing the popup wait for a typed search.

## Keeping docs current

- Adding a module? Copy [`modules/_TEMPLATE.md`](modules/_TEMPLATE.md) to
  `modules/<name>/README.md` and add it to the table above.
- Changing a module's API or behavior? Update its doc **in the same change**.
- Making a significant design decision? Add an ADR using
  [`adr/0000-template.md`](adr/0000-template.md).
