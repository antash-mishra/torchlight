# lexical

> **Status:** Implemented (M3 Part 2; M6 cancellation, Frizbee scoring, cross-query word caches and delta segments; M5 personal boosts): explicit fields, prefix edits, optimal alignment and first-token completion
> **Source:** `src/index/lexical.c` (build), `src/index/lexical_query.c` (search),
> `src/index/lexical_internal.h` · **Header:** `include/torchlight/lexical.h`
> **Tests:** `tests/unit/test_lexical.c`, `tests/alloc/query.c`

## Behavior and ownership

The opaque engine copies exact paths and owns everything built from them. Add
absolute paths with monotonically increasing nonzero ids, then seal with
`lexical_finish`. An allocation failure poisons the builder. The caller destroys
an engine only when no workspaces remain. M2's [catalog](../catalog/README.md)
owns sealed engines and leases workspaces across `catalog_gen` publication.

`lexical_resolve` binary-searches the monotonically ordered file ids in a sealed
engine and returns its exact borrowed path, with no I/O or allocation. Absent
ids return `TL_STATE`; no filesystem existence check is performed.

**Layout.** Entries are a struct of arrays (ids, raw-path offsets, basename
symbols/boundaries, masks, parent directory node) so scans read only the columns
they need. Parent directories live once each in a [dirtree](../dirtree/README.md).
Per entry the engine keeps three 64-bit filters: the basename symbol mask, a
*repeat mask* (symbols occurring twice or more, so `apps` needs two `p`s) and a
*context mask* (basename plus every ancestor), all conservative.
The basename masks also have an immutable [core bitmap index](../core/README.md).
Directory-to-entry lists and descendant counts support complete parent unions
and cheap scan-order estimates.

**Channels**, built at seal time over basenames: [prefix](../prefix/README.md)
(basename, tokens, initials), [trigram](../trigram/README.md),
[typo](../typo/README.md), and a second prefix index over directory names.
Directories above every indexed root (`/home` for root `/home/user`) are not
usable parent context; the root's own name is.

**Matching.** Every whitespace-separated word must match through some channel:

| Evidence for a word | Score |
|---|---|
| basename key prefix: whole basename / token / initials | 6000 / 5000 / 4500 |
| one-edit basename token or eligible unfinished-prefix typo | 3500 |
| explicit generic-name / keyword prefix | 2800 / 2600 |
| parent directory name or token starting with the word | 2500 |
| basename subsequence | 1500 + fuzzy score |
| enough shared basename trigrams | 1000 + 1000 × shared/total |
| subsequence within one usable parent directory name | fuzzy score |
| word containing `/`: subsequence across the full path | fuzzy score |

Complete name/auxiliary token prefixes gain 128, preserving the tier order.
The first basename token keeps basename strength, so its complete match scores
6128 rather than having a 5128 token hit overridden by a 6000 basename prefix.
For example, `project` prefers `project notes.txt` over `projectile.txt` even
though the latter is shorter. Auxiliary fields retain their own weights and
completion bonus. See [ADR 0021](../../adr/0021-first-token-completeness.md).
`lexical_add_fields` copies separate generic-name and keyword fields, with an
indexed prefix channel and mask-filtered fuzzy evidence. Names, metadata and
folder context remain independent. Auxiliary fields are intended for a small
application catalog; files without them incur no scan of fields. Fuzzy proximity
is scored by the [fuzzy](../fuzzy/README.md) module's Frizbee matcher on the
portable scorer's scale (raw bytes for ASCII names, normalized UTF-8 otherwise),
with the portable optimal alignment for words with `/`, invalid UTF-8 or more
than 64 symbols.

A word's score is the best of its evidence; word scores add up. Parent context
is the nearest matching ancestor. Basenames up to 63 symbols gain
`64 - length` so that shorter names win otherwise-equal matches. Exact raw paths
(20,000,000) and exact normalized basenames (10,000,000) have priority. Ties
order by raw path bytes, then id (a precomputed path rank).

An unsealed builder may use `lexical_set_prefix_bonus` to add a bounded per-word
bonus to basename/token/initials prefix hits. The default is zero, preserving
file ranking. Parent, typo, subsequence and trigram evidence receive no bonus.
The setting is fixed before sealing, including the one-symbol result cache;
multiword pruning bounds and the exact-match safety bound include it. M3's
desktop engine uses this generic mechanism to prioritize typed application
names when merging catalogs. See [ADR 0018](../../adr/0018-application-name-ranking.md).

**Query evaluation** (`lexical_query.c`), with no I/O or SQL. The only heap
allocation compiles a Frizbee matcher for a word the workspace has not cached
(once per word and scoring participant, never per entry); repeating a query
allocates nothing:

1. One-symbol queries (`a`–`z`, `0`–`9`) are answered from results computed at
   seal time; they start every typed query and match the most entries.
2. For each word, channel hits are collected per entry. Directory names are
   scored lazily for small batches, sequentially in tree order (over
   contiguous parent and name-mask arrays, scoring only names that hold every
   symbol of the word) for large ones. Four evidence caches keep channel hits,
   directory scores and the word's matchers, keyed by the word's symbols and
   reused across queries with LRU replacement: typing `notes rep` after
   `notes re` recomputes nothing for `notes`. Eviction changes work only; a
   failed or cancelled query drops the caches.
3. Choose the first word using basename-mask counts, possible parent descendants
   and complete cached membership sizes. Estimates choose order only.
   Single- and multiword queries evaluate channel hits before scanning and skip
   only when the heap beats a proven upper bound on every unhit entry (exact).
   Exact multiword basenames are seeded independently. Otherwise scan complete
   cached membership, or intersect basename-mask bitmaps and union channel hits
   and entries with positive parent context. Every possible match is included.
4. Single-word matches stream straight into a bounded heap. Multiword candidates
   are filtered by the other words; candidates matching the first word only
   through a parent directory are deferred and skipped when the heap already
   beats their best possible total (exact).

**Bounded parallel scoring.** Workspaces for engines of at least 65,536 entries
own the coordinator plus three prestarted workers. Non-path batches of at least
4,096 entries score disjoint output positions after all directory context has
been resolved. Membership recording, candidate compaction and heap ordering
stay on the coordinator. Smaller batches and `/` reconstruction are serial.
No threads are created during a query; each participant scores with its own
matcher (prepared before dispatch), and workspace destruction joins its workers. Each concurrent caller needs its own workspace. Thread startup can
return `TL_IO`, documented in the public header.

**Incremental narrowing.** Each workspace records the complete subsequence
membership of the scanned word (basename, parent name or full path). A later
query whose chosen word extends it, with the same `/` mode, scans only those
members plus channel hits outside them; channel hits are not monotone, so they
are always looked up. Results never depend on the cache.

## Tests

Warm workspaces must equal fresh ones while typing, backspacing, adding words
and using `/` words; small capacities must return the head of the full ranking
(exercising skips and deferral). Tests also cover typo/trigram retrieval,
acronym initials, parent context versus scattered letters, root-ancestor
exclusion, the one-symbol cache, Unicode, raw bytes and exact priorities.
First-token regressions cover spaces, underscores, camelCase, acronym boundaries,
case folding and one-symbol answers, at single-result and larger capacities.
Large fixed-width fixtures independently predict scores and id order for
abbreviations, parent words and typos. They exercise worker batches, both result
capacities and warmed membership. Exact multiword names containing internal
separators have a regression against score-bound shortcuts. The separate glibc
allocator test covers long queries and an active 100k-entry worker workspace.
Queries with six distinct words and repeated words exercise evidence eviction
and remain independent of result capacity.
Caller prefix weighting is tested at zero and its upper bound, including
invalid/sealed settings, exact priorities, typed prefixes and multiword capacity
consistency.

## Related

- ADRs: [0006](../../adr/0006-m1-prefix-subsequence-baseline.md),
  [0007](../../adr/0007-partial-scans-and-component-parent-matching.md),
  [0008](../../adr/0008-m1-completion-channels-directories-config.md),
  [0013](../../adr/0013-query-contracts-and-resident-filters.md)
- [Evaluation](../../evaluation.md) for latency, memory and ranking quality.
- Public headers document parameters, lifetimes and error contracts.

M3 adds lexical_add_entry with a directory flag and lexical_is_dir for resident
presentation metadata. A sorted directory-id vector is separate from scoring;
legacy lexical_add and all matching/ranking behavior remain unchanged. Desktop
search reuses an independent engine over localized metadata without teaching
lexical about GTK, XDG or launch commands.

M3 Part 2 adds exhaustive scorer/edit references and a [mixed relevance evaluator](../../../tests/quality/README.md). See [ADR 0020](../../adr/0020-m3-part2-search-quality.md) and [measurements](../../m3-part2-completion.md).

## M4 metadata integration

The entry/context APIs expose immutable sorted metadata and context clipped at
indexed roots. lexical_exactness exposes exact tiers without leaking numeric
score constants to fusion. Query scoring algorithms remain unchanged.

## Cooperative cancellation (M6 step 1)

`lexical_workspace_cancel` attaches a caller-owned atomic flag to a workspace.
`lexical_query` polls it before each scoring batch and every 1024 scored
entries inside parallel batches, returning `TL_CANCELLED` with the workspace
reusable; a cleared or detached flag restores normal answers. The daemon's
search thread uses it to abandon a query the client has already typed past.
See [ADR 0028](../../adr/0028-m6-search-thread-and-persistence-owner.md).

## Frizbee scoring and word caches (M6 step 2)

Basename and directory subsequences are scored by `fuzzy_matcher_score`. The
engine keeps a bit per entry for ASCII basenames and a path-offset sentinel, so
the matcher reads raw bytes (`lexical_ascii_name`); `dirtree_ascii_name` does the
same for directories. Word caches are tested against fresh workspaces through
shared, reordered and evicted words and a cancelled query. See
[ADR 0029](../../adr/0029-m6-frizbee-scoring-and-candidate-volume.md).

## Delta segments (M6 step 3)

`lexical_set_reference(delta, base)` before sealing makes an engine a delta
segment of a base: the base's roots also bound parent context and display
context, and the base's trigram index decides which trigrams are frequent, so
an entry scores exactly as in a full rebuild (tested against one).
`lexical_workspace_exclude` attaches a bitmap of absent positions (tombstones):
excluded entries never become results or candidates, including exact matches,
roots and cached one-symbol answers (a truncated cache falls back to normal
evaluation). Exclusion applies only when ranking, so word evidence and
subsequence caches stay valid. `lexical_slot` and `lexical_id` map ids and
positions. The [catalog](../catalog/README.md) composes segments; see
[ADR 0030](../../adr/0030-m6-incremental-indexing.md).

## Personal boosts (M5)

`lexical_workspace_boost` attaches up to `LEXICAL_MAX_BOOSTED` (4096)
positions, each with a boost of at most `LEXICAL_BOOST_MAX` (2048). The
workspace copies them; the caller keeps its array. A non-empty query then
ranks each boosted entry that matches by its ordinary score plus its boost,
inside its exact tier. Results equal a full evaluation that adds every boost
([ADR 0033](../../adr/0033-m5-personal-ranking.md)).

- **Main pass.** Runs unchanged, with boosted entries treated as excluded,
  so every exact shortcut keeps working. The single-word scan skip still
  counts strong boosted hits: the side pass ranks them at least as high.
- **Side pass.** Scores only the boosted entries: exact paths and names
  first, then each word, and pushes them with their boosts into the same
  heap. This is exact because boosts only raise boosted entries, so an
  unboosted entry outside the main top results cannot enter the
  personalized ones.
- **Pruning.** With a full heap, an entry is dropped before a word is scored
  when even its best case cannot reach the weakest kept result. The best case
  is its channel hit or the word's subsequence bound, plus the later words'
  maxima, its length bonus and its boost. The weakest score only rises, so
  this is exact.
- **One-symbol queries.** These come from the seal-time cache, which gathers
  no word evidence, and gathering it over every key costs milliseconds. So
  the side pass builds *sparse* evidence for the boosted entries alone: their
  name and field keys through `prefix_score`, and parent folders' keys
  lazily per node. The typo and trigram channels need three symbols, so
  nothing is missed. The sparse cache is dropped when the query ends.
- **Parallel scoring.** A side batch of at least 1024 entries is split across
  the scoring workers once the main pass has resolved every directory for the
  word. Otherwise the coordinator scores it.

Static assertions keep boosted ordinary scores below exact names and boosted
exact names below exact paths. Attaching boosts and boosted queries allocate
nothing (`tests/alloc/query.c`). `tests/unit/test_lexical_boost.c` compares
results with brute-force boosted evaluation:

- small engines, with tombstones and fields;
- a 70k engine checked entry by entry;
- a 70k engine with 1500 boosted entries, which takes the parallel path.

At 500k synthetic paths, typing p95 was 4.63 ms without boosts, 4.81 ms with
1000 boosted entries, 4.99 ms with 2048 and 5.13 ms with 4000, on a loaded
reference machine (2026-10-08, `tests/bench/results/2026-10-08-m5-personal.txt`).
