# Torchlight docs

For a plain-language tour of how the whole system works, with diagrams and
charts of measured performance, open [System design](system-design.html) in a browser. For an overview of what was
built and what comes next, open [Built and next](system-overview.html).
The implemented M3 popup follows the [GUI specification](m3-gui-design.md) and
[interactive design preview](m3-gui-preview.html), including Cinnamon X11 focus,
keyboard actions, status/error states and asynchronous IPC requirements.
The GTK popup has its own look, Tekst: teletext's information design on quiet
Nordic surfaces, in light and dark palettes, with Schibsted Grotesk for what you
read and the teletext face Bedstead for what the system says, a page header with
a ticking clock, an inverse-video selection, colored Fastext links and short,
bounded motion; see [ADR 0048](adr/0048-tekst-popup.md), which replaced the
theme-following look of [ADR 0034](adr/0034-theme-following-popup-with-motion.md),
the [native captures](ui/native-results.png) and the
[interactive prototype](designs/tekst/index.html).
Running applications list their open windows beneath their result, with a New
window action ([ADR 0035](adr/0035-open-windows-under-applications.md)). Results
take one line each, with a short folder ([ADR 0036](adr/0036-one-line-results.md)).
Machines without GTK can build and install just the daemon and command line
with `make headless` and `make install-headless` ([ADR 0037](adr/0037-headless-build.md)).
Releases ship as a `.deb` with a signed APT repository that keeps installations up to
date ([ADR 0050](adr/0050-deb-and-signed-apt-repository.md)), and as a portable archive
whose installer sets Torchlight up for one person on other distributions
([ADR 0056](adr/0056-portable-archive-for-other-distributions.md)); [Releasing](release.md)
says how to publish one. [Building](building.md) covers building from source, the
headless build and development commands, and [Configuration](configuration.md) the
configuration file, daemon options and the command line. The [Wayland windows plan](wayland-windows-plan.md) brought the
open-window list from X11 and GNOME to the other Wayland desktops: the wlroots family and
COSMIC through their task bar protocols, KDE Plasma through KWin scripts, and Cinnamon
through an extension (ADRs 0052 to 0054).
The [M7 plan](m7-plan.md) profiles indexing headlessly and plans two
phases. Phase 1 implements full scans only for a reason, with periodic
rescans limited to the folders inotify cannot cover
([ADR 0038](adr/0038-repair-scans-and-metadata-events.md)), no work for
metadata-only writes, and freed memory returned to the OS
([ADR 0039](adr/0039-returning-freed-memory.md)). The
[Phase 1 review fixes](m7-plan.md#phase-1-review-fixes) preserve failed repair
scopes and commit metadata refreshes without publication.
The deletion-compaction optimization in
[ADR 0041](adr/0041-compaction-counts-retired-base-entries.md) was
[removed after reviewing its cost](m7-plan.md#deletion-compaction-removed-2026-10-11).
A [real-desktop review](m7-plan.md#real-desktop-review-2026-10-11) completes
Phase 1: the desktop catalog parses only when a change stamp moves
([ADR 0042](adr/0042-desktop-change-stamps.md)), only mounts near a root cause
full scans and dropped watch slots are reused
([ADR 0043](adr/0043-mount-changes-near-roots-and-watch-slot-reuse.md)), and
memory is measured against a warm steady state
([ADR 0044](adr/0044-m7-memory-acceptance.md)). Phase 2 makes the remaining
full scans cheap: one stat per entry, one long-lived watcher and an in-memory
persisted-state map ([ADR 0040](adr/0040-cheap-full-scans.md)). Its
[health report](reports/m7-phase2-health-report.html) lists every measurement
with reference ranges, red flags and definitions in plain language. Phase 3
closes its open items: event passes settle and a large delta publishes before
its compaction ([ADR 0045](adr/0045-settled-bursts-and-deferred-compaction.md)),
and children prunes walk the path index while the ledger is leaner
([ADR 0046](adr/0046-children-walks-and-a-lean-ledger.md)); see the
[Phase 3 health report](reports/m7-phase3-health-report.html).
The [M8 plan](m8-plan.md) ranks files by place: among similar matches,
shallow files and the person's own folders come before SDKs, package caches
and vendored code, and an `exclude` setting drops whole folders
([ADR 0047](adr/0047-place-ranking-and-excluded-folders.md),
[health report](reports/m8-place-ranking-health-report.html)).
Everyday words still listed SDK internals in duplicate pairs, so folders that
tools manage are now left out for everyone, recognized by the markers their
tools write ([ADR 0062](adr/0062-folders-tools-manage.md)), and a one-word
query lists word matches and tagged settings first, with near matches only as
the fallback ([ADR 0061](adr/0061-word-matches-first.md)). On the live desktop
the index shrank from 213,520 entries to 26,169 and "wifi" lists Network.

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
and [ADR 0022](adr/0022-m4-semantic-foundation.md). M6 implemented worker
separation, the selected Frizbee SIMD matcher and incremental indexing
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
| index | Place prior for file ranking (M8) | [place](modules/place/README.md) |
| index | Native static Potion | [potion](modules/potion/README.md) |
| service | Background semantic queries | [semantic](modules/semantic/README.md) |
| index | Swappable embedder | [embed](modules/embed/README.md) |
| index | Vector search | [vector](modules/vector/README.md) |
| index | Ranking & fusion | [rank](modules/rank/README.md) |
| index | Usage summary (M5) | [usage](modules/usage/README.md) |
| storage | SQLite store | [store](modules/store/README.md) |
| fs | Crawler | [crawl](modules/crawl/README.md) |
| fs | inotify watcher | [watch](modules/watch/README.md) |
| fs | Mount-table snapshots | [mounts](modules/mounts/README.md) |
| ipc | Socket protocol | [ipc](modules/ipc/README.md) |
| service | Resident daemon | [daemon](modules/daemon/README.md) |
| service | Background writer/history | [writer](modules/writer/README.md) |
| service | Incremental delta publication | [delta](modules/delta/README.md) |
| service | Persisted-state map (M7) | [ledger](modules/ledger/README.md) |
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
  icons and a gliding selection, plus a torch sweep and other non-blocking motion;
  its colors and font are superseded by 0048.
- [0035: Open windows under application results](adr/0035-open-windows-under-applications.md)
  lists every running application's open X11 windows beneath its result; Enter
  switches to the most recent window, and New window runs the entry's desktop
  action.
- [0036: One-line results with short folders](adr/0036-one-line-results.md)
  puts each result on one line with a short, right-aligned folder, drops app
  subtitles and the hover tint, and slims the footer to its key hints.
- [0037: Headless build and install](adr/0037-headless-build.md)
  builds and installs the daemon and command line without GTK.
- [0038: Repair scans only where inotify can miss changes](adr/0038-repair-scans-and-metadata-events.md)
  replaces the 30-second full scan with a repair set, an hourly backstop and a
  mount-table signal, and makes metadata-only writes cause no pass or
  publication.
- [0039: Returning freed memory](adr/0039-returning-freed-memory.md)
  trims the heap after a retired full build and sets the daemon's allocator
  policy in `torchlightd`.
- [0041: Count retired base entries toward compaction](adr/0041-compaction-counts-retired-base-entries.md)
  records the withdrawn deletion-compaction optimization, its measured cost
  and the restored live-delta threshold.
- [0042: Desktop catalog change stamps](adr/0042-desktop-change-stamps.md)
  parses `.desktop` files only when a cheap stat stamp of the applications
  trees changes, instead of every second.
- [0040: Cheap full scans: one stat, one long-lived watcher, a persisted-state map](adr/0040-cheap-full-scans.md)
- [0043: Full scans only for mounts near a root; reuse dropped watch slots](adr/0043-mount-changes-near-roots-and-watch-slot-reuse.md)
  ignores mount-table changes away from the indexed roots (hourly backup
  checks, snaps) and frees the watch slots of deleted directories.
- [0044: M7 memory acceptance against a warm steady state](adr/0044-m7-memory-acceptance.md)
  measures retained free heap and RSS after a rebuild against warm steady RSS,
  and records deleted-entry retention as a bounded limit.
- [0045: Settled event passes and deferred compaction](adr/0045-settled-bursts-and-deferred-compaction.md)
  rescans a burst once it pauses (at most 1 s later) and publishes a delta up
  to four times its bound at once, compacting it when the writer is idle.
- [0046: Children prunes by index walk, and a lean ledger](adr/0046-children-walks-and-a-lean-ledger.md)
  prunes a folder's children with one step per child instead of one per row
  below it, and fingerprints paths in one pass with 4-byte index slots.
- [0047: Rank files by place, and let people exclude folders](adr/0047-place-ranking-and-excluded-folders.md)
  gives each file a small penalty for depth and for lying inside installed or
  vendored software, inside its match tier, and adds an `exclude` setting.
- [0048: Tekst, the popup's own look](adr/0048-tekst-popup.md)
  replaces the theme-following popup with teletext's information design on
  Nordic light and dark palettes, Schibsted Grotesk and Bedstead type, an idle
  index strip, Fastext links with Copy path and a CRT switch-off; high contrast
  keeps the theme's colors.
- [0049: The popup on Wayland](adr/0049-popup-on-wayland.md) sizes the popup for its
  monitor on Wayland as on X11, leaves placement to the compositor (no layer-shell),
  and adds a headless Weston acceptance test.
- [0051: Open windows on GNOME Wayland](adr/0051-open-windows-on-gnome-wayland.md) lists
  and switches windows on GNOME's Wayland session through a small GNOME Shell extension
  that the package installs and the popup enables once.
- [0050: A .deb and a signed APT repository](adr/0050-deb-and-signed-apt-repository.md)
  packages Torchlight for Mint 22 and Ubuntu 24.04+, publishes releases from a tag to a
  signed APT repository on GitHub Pages, and has the daemon and popup restart onto
  updates.
- [0052: Open windows through Wayland task bar protocols](adr/0052-open-windows-through-wayland-taskbar-protocols.md)
  lists and switches windows on Sway, Hyprland, labwc, Wayfire, niri and COSMIC through
  the compositor's task bar protocol, keeping the order of use from activations, and
  chooses each session's window source by what it offers.
- [0053: Open windows through KWin scripts](adr/0053-open-windows-through-kwin-scripts.md)
  lists and switches windows on KDE Plasma 5.27 and 6 with short scripts that KWin runs
  and that report back over D-Bus.
- [0054: Open windows on Cinnamon's Wayland session](adr/0054-open-windows-on-cinnamon-wayland.md)
  ports the GNOME Shell extension to Cinnamon, which the popup turns on once, and gives
  the popup a fresh surface for each showing where GTK cannot release the old one.
- [0055: The search time instead of the result count](adr/0055-search-time-in-the-page-header.md)
  shows how long each search took in Tekst's header, in cyan, at a fixed width, and drops
  the header's result count.
- [0056: A portable archive for other distributions](adr/0056-portable-archive-for-other-distributions.md)
  ships a tar.gz whose installer sets Torchlight up for one person in `~/.local`, without
  root, checked on five distributions before each release.
- [0057: Applications are listed as the desktop's own menu lists them](adr/0057-applications-the-desktop-menu-lists.md)
  follows Cinnamon's menu, which lists GNOME-only entries such as GNOME Terminal, in the
  catalog and the popup's launch check.
- [0058: Applications the query names come before files](adr/0058-applications-the-query-names-come-first.md)
  lists an application ahead of files named exactly like the query when every typed word
  starts its name, a word of it or its initials.
- [0059: The daemon starts at login on desktops without graphical-session.target](adr/0059-daemon-starts-at-login-on-every-desktop.md)
  adds an autostart entry that starts the daemon's unit on Cinnamon, Xfce, MATE and LXQt,
  which sessions run by systemd skip.
- [0060: Later CamelCase words match when typed whole](adr/0060-whole-camelcase-words.md)
  gives a later word that changes case inside (`SQLite`) a prefix key of its own, so
  `sqlite` finds DB Browser for SQLite.
- [0061: Word matches first; near matches only when nothing else matches](adr/0061-word-matches-first.md)
  drops a one-word query's typos and scattered-letter matches once anything matches by a
  word start, and lists applications tagged with the word ("wifi" for Network) after exact
  file names and before other results.
- [0062: Leave out folders that tools manage](adr/0062-folders-tools-manage.md)
  skips virtual environments, Conda, Android SDKs, Go's module cache, tagged caches and
  dependency folders for everyone, by the markers their tools write.
- [0064: The popup's window stays clear and keeps its height on X11](adr/0064-clear-window-steady-height-on-x11.md)
  keeps theme colours out of the window around the surface, holds a see-through X11
  window at the popup's full height so mutter never shows it mid-resize, and marks the
  surface opaque before GTK 4.16.
- [0065: The first edit after a pause searches at once](adr/0065-first-edit-searches-at-once.md)
  sends a key's query without the 16 ms coalescing window when nothing is pending, and
  still coalesces bursts.

## Keeping docs current

- Adding a module? Copy [`modules/_TEMPLATE.md`](modules/_TEMPLATE.md) to
  `modules/<name>/README.md` and add it to the table above.
- Changing a module's API or behavior? Update its doc **in the same change**.
- Making a significant design decision? Add an ADR using
  [`adr/0000-template.md`](adr/0000-template.md).
