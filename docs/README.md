# Torchlight docs

For a plain-language tour of how the whole system works, with diagrams, open
[System design](system-design.html) in a browser. For an overview of what was
built and what comes next, open [Built and next](system-overview.html).
The implemented M3 popup follows the [GUI specification](m3-gui-design.md) and
[interactive design preview](m3-gui-preview.html), including Cinnamon X11 focus,
keyboard actions, status/error states and asynchronous IPC requirements.
The GTK popup follows the desktop theme (light or dark, accent and font) with
short, bounded motion; see [ADR 0034](adr/0034-theme-following-popup-with-motion.md),
the [design study](popup-directions.html) and [native captures](ui/native-results.png).
Running applications list their open windows beneath their result, with a New
window action ([ADR 0035](adr/0035-open-windows-under-applications.md)). Results
take one line each, with a short folder ([ADR 0036](adr/0036-one-line-results.md)).

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
The [M3 review fixes](adr/0017-m3-snapshot-cancellation-and-acceptance.md) cover
consistent desktop replacement, action cancellation, keyboard Retry, accessible
input and scaled placement. [Milestone status](milestone-status.md) explains
what is done and what remains in plain language. M3 Part 2 search quality is
implemented. M4 implements opt-in native Potion, float/int8 retrieval, versioned background
caching and coherent two-phase RRF. Large-catalog latency and broader model
acceptance remain open. See [M4 implementation](m4-implementation.md)
and [ADR 0022](adr/0022-m4-semantic-foundation.md). M6 executes next: worker
separation, then the selected Frizbee SIMD matcher, then incremental indexing,
as three steps in one milestone. M5 recommendations are implemented, as
[ADR 0033](adr/0033-m5-personal-ranking.md) describes; semantic
search is parked as opt-in and its M4 acceptance is deferred
([ADR 0032](adr/0032-park-semantic-search.md)). See [ADR 0026](adr/0026-search-workers-simd-and-incremental-indexing.md)
and the [M6 working plan](m6-plan.md), which has the target thread/flow diagram,
the measured baseline and per-step exit criteria. All three M6 steps are now
implemented: the search thread ([ADR 0028](adr/0028-m6-search-thread-and-persistence-owner.md)),
Frizbee scoring with cross-query word caches ([ADR 0029](adr/0029-m6-frizbee-scoring-and-candidate-volume.md))
and incremental indexing with scoped rescans, delta segments and incremental
semantic stages ([ADR 0030](adr/0030-m6-incremental-indexing.md)).
Remaining M4 acceptance work started with a two-pass prefix-shortlist vector
search ([ADR 0031](adr/0031-m4-prefix-shortlist-vector-search.md)).
[Part 2 verification](m3-part2-completion.md) records indexed prefix edits,
explicit metadata fields, optimal fuzzy alignment, held-out/BM25 comparisons,
latency and RSS. The [search quality review](search-quality.md) explains the
original Chrome/prefix-typo failures and future M4 model selection.
[ADR 0018](adr/0018-application-name-ranking.md) fixes application-name weighting;
[ADR 0019](adr/0019-search-quality-before-semantic-personalization.md) records the
staged sequence, implemented for Part 2 in [ADR 0020](adr/0020-m3-part2-search-quality.md).

M3 proceeded with the user's acceptance of roughly 6 ms p95 at 500k paths.
The original 5 ms lexical target and full-engine rebuild cost are now tracked in
M6. The [readiness report](m3-readiness.md) records pre-M3 verification
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
| index | Native static Potion | [potion](modules/potion/README.md) |
| service | Background semantic queries | [semantic](modules/semantic/README.md) |
| index | Swappable embedder | [embed](modules/embed/README.md) |
| index | Vector search | [vector](modules/vector/README.md) |
| index | Ranking & fusion | [rank](modules/rank/README.md) |
| index | Usage summary (M5) | [usage](modules/usage/README.md) |
| storage | SQLite store | [store](modules/store/README.md) |
| fs | Crawler | [crawl](modules/crawl/README.md) |
| fs | inotify watcher | [watch](modules/watch/README.md) |
| ipc | Socket protocol | [ipc](modules/ipc/README.md) |
| service | Resident daemon | [daemon](modules/daemon/README.md) |
| service | Background writer/history | [writer](modules/writer/README.md) |
| service | Incremental delta publication | [delta](modules/delta/README.md) |
| service | Personal ranking state (M5) | [personal](modules/personal/README.md) |
| bin | CLI client | [cli](modules/cli/README.md) |
| ui | GTK4 popup & optional TUI | [ui](modules/ui/README.md) |
| ui | Open windows under applications | [windows](modules/windows/README.md) |

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
- [0017: M3 snapshots, cancellation and acceptance](adr/0017-m3-snapshot-cancellation-and-acceptance.md)
  fixes replacement and action races and adds isolated GUI/accessibility/paint checks.
- [0018: application names in mixed launcher results](adr/0018-application-name-ranking.md)
  prioritizes name prefixes and preserves tie ordering across result limits.
- [0019: search quality before semantics and personalization](adr/0019-search-quality-before-semantic-personalization.md)
  plans M3 Part 2 before M4 hybrid search and M5 personal recommendations.

- [0020: M3 Part 2 search quality](adr/0020-m3-part2-search-quality.md)
  adds explicit fields, indexed prefix edits and bounded optimal alignment.
- [0021: first-token completion](adr/0021-first-token-completeness.md)
  preserves the completion bonus at basename strength without growing the index.
- [0022: M4 semantic foundation](adr/0022-m4-semantic-foundation.md)
  starts the swappable embedder, float cosine reference and bounded exact-priority RRF.
- [0023: native Potion and two-phase search](adr/0023-m4-native-potion-and-two-phase-search.md)
  records the provisional backend, owned metadata snapshots and measured limits.
- [0024: M4 response backpressure and model inputs](adr/0024-m4-response-backpressure-and-model-inputs.md)
  keeps large two-phase responses bounded, preserves status and rejects blocking model streams.
- [0025: M4 cache staging retries](adr/0025-m4-cache-staging-retries.md)
  retains partial embedding builds when cache transactions cannot begin during reconciliation.
- [0026: search workers, SIMD and incremental indexing](adr/0026-search-workers-simd-and-incremental-indexing.md)
  appends one M6 milestone with worker separation, selected Frizbee integration
  and incremental indexing in that order; M6 executes before M5.

- [0027: Quiet System native popup](adr/0027-quiet-system-native-popup.md)
  implemented the minimal layout, shaped underscore cursor and typing light without
  shaders; its presentation is superseded by 0034.
- [0028: M6 step 1, search thread and persistence owner](adr/0028-m6-search-thread-and-persistence-owner.md)
  moves search off the IPC thread with per-client queues and cancellation, and
  splits the writer into indexing and persistence threads.
- [0029: M6 step 2, Frizbee scoring and candidate volume](adr/0029-m6-frizbee-scoring-and-candidate-volume.md)
  vendors Frizbee, scores on the portable scale, and reuses word evidence across
  queries; typing p95 at 500k drops below 5 ms with unchanged recall.
- [0030: M6 step 3, incremental indexing](adr/0030-m6-incremental-indexing.md)
  rescans only event directories, publishes deltas over a shared base with
  compaction, and reuses semantic vectors for unchanged rows.
- [0031: M4 acceptance, prefix-shortlist vector search](adr/0031-m4-prefix-shortlist-vector-search.md)
  shortlists rows by their Matryoshka prefix and rescores them exactly: 0.9956
  recall@10 against exhaustive int8 at 500k with a four times faster scan.
- [0032: Park semantic search](adr/0032-park-semantic-search.md)
  keeps semantic search opt-in and tested, defers its M4 acceptance gates and
  moves M5 next.
- [0033: M5 personal ranking without slowing search](adr/0033-m5-personal-ranking.md)
  keeps usage in search-thread memory, all saving on the persistence thread,
  and adds bounded boosts through an exact used-set side query (implemented).
- [0034: Theme-following popup with bounded motion](adr/0034-theme-following-popup-with-motion.md)
  replaces the Quiet System look with theme colors, the desktop font, file-type
  icons and a gliding selection, plus a torch sweep and other non-blocking motion.
- [0035: Open windows under application results](adr/0035-open-windows-under-applications.md)
  lists every running application's open X11 windows beneath its result; Enter
  switches to the most recent window, and New window runs the entry's desktop
  action.
- [0036: One-line results with short folders](adr/0036-one-line-results.md)
  puts each result on one line with a short, right-aligned folder, drops app
  subtitles and the hover tint, and slims the footer to its key hints.

## Keeping docs current

- Adding a module? Copy [`modules/_TEMPLATE.md`](modules/_TEMPLATE.md) to
  `modules/<name>/README.md` and add it to the table above.
- Changing a module's API or behavior? Update its doc **in the same change**.
- Making a significant design decision? Add an ADR using
  [`adr/0000-template.md`](adr/0000-template.md).
