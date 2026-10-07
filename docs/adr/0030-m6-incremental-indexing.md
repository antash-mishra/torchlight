# 0030. M6 step 3: incremental indexing

- **Status:** Accepted; implemented
- **Date:** 2026-10-07

## Context

After step 1 any filesystem change still crawled every root, rebuilt the whole
lexical engine from SQLite and published it: 4.0 s of update lag for 100
touched files at 500k entries, with RSS rising from 277 to 497 MB and peaking
at 677 MB. The semantic worker then copied every path again and looked every
row up in the embedding cache before hybrid search could serve the new
`catalog_gen`. The [M6 plan](../m6-plan.md) splits the fix into scoped
reconcile (3a), a segmented engine (3b) and incremental semantic snapshots (3c).

## Decision

**3a. Scoped reconcile.** Watch events now carry `created` (created, or moved
in without a paired source). The writer turns each event into directory
scopes: the parent of the changed entry (and of a rename's source) is rescanned
for its direct children; a created directory is rescanned recursively.
`crawl_scope` applies the root walk's exclusion, allowlist and
unreadable-scope rules below one indexed directory. `store_prune_children` and
`store_prune_tree` delete unseen children (with subtrees) or unseen descendants
through a path-index range, sparing kept scopes and nested roots. New
directories get watches in the live watcher. A full scan still runs at
startup, for overflow, explicit requests, periodic repair, failures, more than
1024 scopes, a missing watcher, and any scope above a root (a root itself was
removed or moved).

**3b. Segmented engine.** A catalog snapshot is a base engine shared,
reference-counted, by every snapshot derived from it, plus an optional delta
engine of all entries changed since that base and a tombstone bitmap of base
positions they replace or remove. The base owns the reader workspaces, so word
caches stay warm across small updates. Queries run on both segments and merge
in the engines' total order (score, raw path, id). The delta is built with
`lexical_set_reference(base)`: the base's roots bound parent context and its
trigram index decides which trigrams are frequent, so an entry scores the same
in either segment. `lexical_workspace_exclude` keeps tombstoned entries out of
results, exact matches, roots and cached one-symbol answers, while word
evidence and subsequence caches stay valid.

The store's SQLite update hook records the files rows each catalog transaction
inserts, updates or deletes. After a commit the persistence thread hands those
ids to the indexing thread, which loads the committed rows (`store_load_ids`),
merges them with the kept delta entries (the `delta` module) and derives the
next snapshot. A full rebuild compacts instead when the delta would exceed
max(4096, base/32) entries, when tracking overflowed (more than 131072 rows),
when registered roots changed, or when the active view is not the writer's last
publication. Full rebuilds remain the startup, recovery and compaction path.

**3c. Incremental semantic snapshot.** Each staged row keeps a 64-bit hash of
its prepared embedding text, and a row whose text is unchanged copies its
stored vector (`vector_add_row`, bit-identical) instead of being embedded.
Semantic snapshots are segmented like catalog snapshots. When the published
snapshot uses the same model and its catalog view shares the new view's base,
the stage is derived: it references the published snapshot's full base, re-reads
only the ids in both views' change sets (`catalog_snapshot_changes`, plus every
application when the desktop catalog changed), owns rows for changed and new
ids, and hides replaced or removed base entries and vector rows in two bitmaps.
Queries search both vector segments, the base through a workspace that skips
hidden rows, and merge the top lists by cosine and id. A derived stage that
would own more than max(4096, base/32) rows, a new catalog base or a model
change stages a full snapshot, copying vectors from both segments of the
published one. Only stages that reuse nothing re-stage the model descriptor and
sweep the cache, because reused rows never touch their cache entries.

## Alternatives considered

- Updating the base engine in place would mutate structures leased by readers.
- Copy-on-write blocks inside every channel (prefix keys, postings, typo and
  mask indexes) would avoid compaction but touch every module's layout.
- Diffing the crawl against `store_load` would still read the whole catalog per
  update; the update hook gives the exact change set for free.
- Recomputing frequent-trigram decisions over both segments would need global
  counts per query; taking them from the base is consistent and exact again
  after each compaction.
- Copying every row and vector into each stage was implemented first: it
  removed all SQLite lookups and inference for unchanged rows, but at 500k
  entries it copied 500k metadata rows per update and raised RSS from 602 to
  835 MB, because the old and new snapshots coexist until jobs drain.

## Consequences

At 500k entries, 100 touched files publish in about 110 ms (the reconcile
itself takes 11 to 13 ms; the rest is the 100 ms coalescing window), down from
4.0 s, and RSS grows about 1% without a new peak. With a model, hybrid search
serves the update after 277 ms through a derived semantic snapshot (797 ms
when every stage copied all rows), and RSS goes from 603 to 605 MB instead of
602 to 835 MB. Snapshots derived from one base cost a delta engine, a tombstone
bitmap and a live-entry map (4 bytes per entry); derived semantic snapshots
cost their own rows plus two bitmaps, and a hybrid query scans two vector
segments. Between compactions, frequent-trigram decisions come from the base,
and deleted paths' embedding cache rows stay until the next full semantic
stage. Status reports `scoped_reconciliations`, `delta_publications`,
`full_builds` and `semantic.reused`, `derived_stages` and `full_stages`. Tests cover scoped crawls and prunes, change sets, delta
scores equal to a full rebuild, tombstone exclusion, merged ordering, compaction,
renames and moved-in trees without periodic repair, incremental semantic
reuse, and derived semantic snapshots (hidden base rows, base metadata, a full
stage reusing both segments).
