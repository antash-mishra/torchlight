# lexical

> **Status:** Implemented (M1): prefix, subsequence, trigram and typo channels
> **Source:** `src/index/lexical.c` (build), `src/index/lexical_query.c` (search),
> `src/index/lexical_internal.h` · **Header:** `include/torchlight/lexical.h`
> **Tests:** `tests/unit/test_lexical.c`

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

**Channels**, built at seal time over basenames: [prefix](../prefix/README.md)
(basename, tokens, initials), [trigram](../trigram/README.md),
[typo](../typo/README.md), and a second prefix index over directory names.
Directories above every indexed root (`/home` for root `/home/user`) are not
usable parent context; the root's own name is.

**Matching.** Every whitespace-separated word must match through some channel:

| Evidence for a word | Score |
|---|---|
| basename key prefix: whole basename / token / initials | 6000 / 5000 / 4500 |
| one-edit basename token typo | 3500 |
| parent directory name or token starting with the word | 2500 |
| basename subsequence | 1500 + fuzzy score |
| enough shared basename trigrams | 1000 + 1000 × shared/total |
| subsequence within one usable parent directory name | fuzzy score |
| word containing `/`: subsequence across the full path | fuzzy score |

A word's score is the best of its evidence; word scores add up. Parent context
is the nearest matching ancestor. Basenames up to 63 symbols gain
`64 - length` so that shorter names win otherwise-equal matches. Exact raw paths
(20,000,000) and exact normalized basenames (10,000,000) have priority. Ties
order by raw path bytes, then id (a precomputed path rank).

**Query evaluation** (`lexical_query.c`), with no I/O, SQL or heap allocation:

1. One-symbol queries (`a`–`z`, `0`–`9`) are answered from results computed at
   seal time; they start every typed query and match the most entries.
2. For each word, channel hits are collected per entry; directory names are
   scored lazily and at most once per word.
3. The first word is the one extending the cached membership, else the longest.
   A single-word query whose channel hits already fill the results with scores
   above every possible subsequence score skips the scan entirely (exact).
   Otherwise every entry (or only the cached membership) is scored; the context
   mask rejects most entries with one load.
4. Single-word matches stream straight into a bounded heap. Multiword candidates
   are filtered by the other words; candidates matching the first word only
   through a parent directory are deferred and skipped when the heap already
   beats their best possible total (exact).

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

## Related

- ADRs: [0006](../../adr/0006-m1-prefix-subsequence-baseline.md),
  [0007](../../adr/0007-partial-scans-and-component-parent-matching.md),
  [0008](../../adr/0008-m1-completion-channels-directories-config.md)
- [Evaluation](../../evaluation.md) for latency, memory and ranking quality.
- Public headers document parameters, lifetimes and error contracts.
