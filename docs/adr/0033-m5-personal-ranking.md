# 0033. M5 personal ranking without slowing search

- **Status:** Accepted; implemented
- **Date:** 2026-10-08

## Context

M5 ranks the files and applications a user opens higher. History recording,
clearing and retention already exist (ADRs 0011 and 0015), but nothing uses
them for ranking. Three facts about the current code shape the design:

- `lexical_query` keeps only the requested number of results (ten for the
  popup) in a bounded heap. Re-ranking its output can only reorder those ten;
  a file opened daily that ranks 14th never appears.
- The engine's exact shortcuts depend on score bounds. `try_skip_scan` skips
  the full scan once `capacity` channel hits beat the subsequence bound, the
  multiword and deferred-parent paths use similar bounds, and one-symbol
  queries are answered from results computed when the engine is sealed.
  Fetching hundreds of results to re-rank, or adding boosts inside entry
  scores, weakens these shortcuts for every query.
- The search thread already runs each client's queries, resolves, opens and
  history clears in order (ADR 0028), and the persistence thread owns the
  SQLite write connection. Today every query, including each keystroke, is
  queued as a `searches` row and written in its own transaction.

## Decision

1. **Signals.** Frecency per file id and per application desktop id: one
   exponentially decayed open count per item, with the half-life a named
   constant, updated in O(1) per open and never recomputed from `opens`
   during a query. Query-to-open history: (normalized query, opened target)
   pairs; a typed query boosts the targets of stored queries that start with
   it, held in a sorted array searched by prefix range. Both are capped
   (initially about 2048 items and 4096 pairs) and evict the weakest entry
   when full. Application usage is keyed by desktop id, because session
   application ids are retired on every desktop refresh.
2. **Threads.** The search thread alone owns the resident usage summary as
   plain memory: no locks and no published snapshot. An accepted open updates
   it in memory and is queued for saving; the search thread never runs SQL or
   waits for a write. Usage summaries are not part of a `catalog_gen`, so an
   open never republishes the catalog. The persistence thread does all saving,
   retention pruning and the startup load. At startup it reads retained
   history before draining newly queued events, builds the summary and hands
   it over with one pointer exchange. Until then queries run unboosted, and
   the search thread then folds the opens it accepted meanwhile into the
   loaded summary, so none is lost or counted twice.
3. **Saving.** The persistence thread drains queued history in one
   transaction with cached prepared statements. The search thread keeps each
   client's last few (search id, query) pairs in memory; a search row is
   saved only together with an open that references it, in the same
   transaction. No schema change: the summary is rebuilt from retained
   `opens`, `desktop_opens` and `searches` rows.
4. **Ranking with a used-set side query.** The normal query excludes used
   entries through a second exclusion bitmap beside the delta tombstones and
   otherwise runs unchanged, keeping every shortcut. A side query scores only
   the used entries with the same channels and word filters, adds their
   boosts, and the two lists merge into the top k. This is exact: boosting
   other entries can only push an unused entry down, so the personalized top
   k is drawn from the unused top k plus the scored used entries. Merging
   instead of excluding would not be exact, because a used entry in the
   unused list would take the slot of the entry that should follow. Used ids
   map to base and delta positions once per `catalog_gen`, and to desktop
   entries once per desktop refresh, never per keystroke. The desktop engine
   uses the same mechanism.
5. **Bounds.** Boost caps are named constants with static assertions, on the
   single score scale shared by the base, delta and desktop engines (their
   results are merged by raw score). Frecency stays below the smallest gap
   between channel tiers (initials 4500 to token prefix 5000), so it reorders
   matches of similar strength such as Chrome and `chapter.pdf` for `ch`.
   Query-to-open may cross about one tier. Both stay below the exact-name
   tier. Exact matches are also ordered by boost, so the opened `README.md`
   leads duplicate exact names. With a model, RRF inherits the boosts through
   lexical ranks; the parked semantic path needs no change.
6. **Controls.** Disabled history creates no summary, and ranking is
   identical to today's. Clearing history empties the summary on the search
   thread before the clear is answered and discards a pending startup load.
   An entry whose last open is older than retention gives no boost and is
   evicted first. The empty popup stays as ADR 0016 describes.

Acceptance criteria are listed under M5 in `PLAN.md`.

## Alternatives considered

- **Re-rank the returned top results.** Cheap, but it cannot promote an entry
  that missed the cut, which is the main point of personalization.
- **Fetch a larger pool (100 to 200) and re-rank.** Still inexact beyond the
  pool, and a larger capacity stops `try_skip_scan` from skipping the full
  scan, slowing every keystroke.
- **Add boosts inside lexical scores.** Every exact bound must include the
  maximum boost, weakening pruning for all queries, and seal-time one-symbol
  answers would be stale after each open.
- **Keep usage summaries in each `catalog_gen`** (the earlier `PLAN.md`
  wording). Every open would republish the catalog, about 110 ms of delta
  build at 500k, for a ranking hint.
- **A locked or published usage snapshot.** Unneeded while one search thread
  is its only reader and writer.
- **Keep saving every keystroke's search.** More transactions and disk writes
  for rows that no feature reads.

## Consequences

- Each keystroke costs O(used entries) more, bounded by the caps; M5 measures
  it at 500k with 0, 1000 and 4000 used entries against the 5 ms typing
  target.
- The lexical engine gains an API that scores a given list of entries and a
  second exclusion bitmap per workspace.
- `searches` keeps only searches that led to an open; searches without an
  open are no longer saved.
- The startup rebuild reads retained history on the persistence thread; its
  cost is bounded by retention and stays off the search path.
- If search is ever split across several threads, the summary must become a
  published immutable snapshot.

## Implementation notes (2026-10-08)

Implemented as decided, in the `usage` (index) and `personal` (service)
modules, with boosted lexical, catalog and desktop queries. Measuring the
side pass at 500k changed four details, all exact:

- **Pruning.** The side pass drops an entry before a word is scored when even
  its best case cannot reach the weakest kept result.
- **Scan skip.** The single-word scan skip counts strong boosted hits too, so
  excluding a few strong entries no longer forces a full scan.
- **One-symbol queries.** These come from the seal-time cache. Gathering
  their evidence over every key made a first keystroke take 7–9 ms with
  boosts, so the side pass now builds sparse evidence for the boosted
  entries alone, through a new `prefix_score` that shares the prefix
  module's key enumeration.
- **Parallel scoring.** Side batches of 1024 or more entries use the scoring
  workers once every directory is resolved.

Results at 500k (p95 typing): 4.63 ms without boosts, 4.81 ms with 1000
boosted entries, 4.99 ms with 2048 (the summary's cap) and 5.13 ms with 4000.
In the synthetic usage scenario, habitual files reach first place after 3.47
keystrokes instead of 9.94; files never opened are unchanged (11.67 → 11.54).
See [evaluation](../evaluation.md#m5-personal-ranking-2026-10-08).
