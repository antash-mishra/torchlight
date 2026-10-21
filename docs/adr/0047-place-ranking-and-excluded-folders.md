# 0047. Rank files by place, and let people exclude folders

- **Status:** Accepted; its rejected default exclusions are revised by
  [ADR 0062](0062-folders-tools-manage.md), which leaves out folders that tools manage
- **Date:** 2026-10-12

## Context
File ranking used match strength alone. Equal matches were ordered by a
64-point short-name bonus and then alphabetically by full path. Nothing said
where a file lives. On the live desktop (213,356 entries) 66% of entries are
under `~/Android/Sdk` and 18% under the Go module cache (`~/go/pkg`), so they
won almost every tie:

- Typing `sc`, the ten file results were all folders deep in the Android NDK,
  the Go module cache and a cloned library. `/home/antash/Android/...`
  sorted first because uppercase `A` sorts before every lowercase letter.
- Typing `scripts` exactly, the first three results were `venv/scripts`
  folders inside the Android NDK and a downloaded tool. The person's own
  `~/workspace/torchlight/scripts` was not among them.
- Across 30 everyday queries, 186 of the 300 top-10 file results were inside
  third-party trees and 78 were in the person's own folders.

There was also no way to drop a tree from the index short of listing every
other root.

## Decision

**A place penalty, computed once per directory.** A new `place` module holds
the policy as pure functions over normalized directory names. When a file
engine is sealed, `lexical` works out each interned directory's penalty once
and stores it for every entry inside (16 bits per entry, so a scored entry
costs one extra load), and a query subtracts it from each result:

- **Depth.** 24 per directory beyond three between the indexed root and the
  entry (`~/workspace/project/src/x` is free).
- **Dependency trees.** 128 more below a folder that by convention holds
  software installed or vendored by a tool: `node_modules`,
  `bower_components`, `site-packages`, `dist-packages`, `vendor`,
  `third_party`, `third-party`, `thirdparty`, `sysroot`, `sdk`, Go module
  folders (`name@v1.2.3`, `@v`) and versioned install folders (`27.0.12077973`,
  `1.4.335.0`; not dotted dates such as `2024.05.01`). The marker folder
  itself is not penalized, so `site-packages` still finds the folder.
- **Cap 256**, below the smallest gap between channel tiers (initials 4500 to
  token prefix 5000), as ADR 0033 caps frecency. A penalty reorders matches of
  similar strength. It never moves a stronger kind of match below a weaker one,
  and an exact name always stays above every non-exact match. It may
  outweigh the 64-point short-name bonus and the 128-point completion bonus,
  which are weaker signals than place.

Ordinary results score `match + short-name bonus − penalty`, at least 1. Exact
names score the exact-name tier minus the penalty, and `lexical_exactness`
still classifies them. Exact raw paths and indexed roots are not penalized.

A penalty only lowers scores. Every pruning shortcut compares an upper bound
for unscored entries with real scores, so all bounds stay valid without
change. The one count of "strong" hits that lets a single-word query skip its
scan uses penalized scores. Seal-time one-symbol answers run through the same
path after the penalties exist.

Place ranking is a per-engine setting, off by default
(`lexical_set_place_ranking`). The daemon's file catalog, the CLI, the
benchmark and the quality evaluator turn it on; the desktop catalog does not.
A delta segment inherits it from its reference engine. A directory's place
depends only on its path and the indexed roots (the reference's roots bound a
delta too), so an entry gets the same penalty in a delta as after a full
rebuild, and segments still merge by raw score.

**`exclude` in the configuration.** `exclude = PATH` (repeatable, `~/`
expands) is accepted by `torchlight index` and `torchlightd`. The crawler takes
a list of excluded scopes instead of the single state directory. Every
excluded scope wins, even below a root or an allowlisted directory, so one
decision function still serves walks, scoped walks and coverage checks. An
excluded tree is never reported or watched, so the next full scan prunes its
saved entries. As with `root` and `allow`, the daemon applies a changed
configuration when it restarts.

## Alternatives considered

- **Break ties by place only.** Not enough: for `sc` the eight four-letter
  names tie above every longer name, and seven of them were third-party.
  Place has to be able to outweigh the short-name bonus.
- **A larger cap (400 to 600).** In a simulation on the live catalog with 50
  queries, caps from 256 to 600 left 187 to 192 of the 500 top-10 results in
  third-party trees (315 before), because most of the gain is reordering
  near-ties. The smallest cap disturbs match quality least.
- **A positive bonus for good places.** Every pruning bound would have to add
  the maximum bonus, weakening the exact shortcuts for every query (ADR 0033
  rejected boosts inside lexical scores for this reason).
- **Default exclusions** for SDKs and module caches. Hiding them would make
  them unfindable; a penalty keeps them one more keystroke away. People who
  never want them can exclude them.
- **Learning folder affinity from opens**, and **recency from modification
  times**. Both are useful and planned separately: affinity needs a
  per-folder weight (per-entry boosts are capped at 4096), recency needs a
  resident time column.
- **Detecting third-party trees by size** (folders with many descendants
  nobody opens). Descendant counts change with every delta, so a delta could
  no longer score like a rebuild.

## Consequences

- Shallow files and the person's own folders lead their match tier; SDK,
  package and vendored files move down without disappearing. Results are
  measured in the [M8 plan](../m8-plan.md) and the
  [M8 health report](../reports/m8-place-ranking-health-report.html).
- A deep file the person does use (a `vendor` folder they work in) ranks a
  little lower until personal boosts (ADR 0033) lift it.
- The marker list is policy and will need entries for other ecosystems.
- Each file engine holds 2 bytes per entry more (0.4 MiB at 213k, 1 MiB at
  500k). Stored per directory it would be 70 KiB, but every scored entry
  would need a second scattered load: typing p95 at 500k was 3% higher that
  way, against 0.4% per entry (18 alternating runs).
- A delta takes trigram frequencies from its base, so a few weak trigram
  matches can differ from a full rebuild until compaction. This predates M8
  and is independent of place ranking; the place tests compare only entries
  whose unpenalized scores agree.
