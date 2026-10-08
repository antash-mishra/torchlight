# usage

> **Status:** Implemented (M5)
> **Source:** `src/index/usage.c` · **Header:** `include/torchlight/usage.h`
> **Tests:** `tests/unit/test_usage.c`, `tests/bench/personal.c`

## Purpose

The resident usage summary behind M5 personal ranking
([ADR 0033](../../adr/0033-m5-personal-ranking.md)). It remembers which files
and applications a person opens, how often and how recently, and which typed
queries led to them, and turns that into a bounded boost per item for each
query.

## Responsibilities

- Owns a capped table of items (a file id or an application desktop id) with
  decayed open counts, and a capped, sorted table of query-to-open pairs.
- Computes per-query boosts on the lexical score scale.
- Does **not** save anything, load history or know about threads: the
  [writer](../writer/README.md) rebuilds a summary from SQLite at startup and
  the daemon's [personal](../personal/README.md) state owns the live one.
- Does **not** rank: the [lexical](../lexical/README.md) side pass applies the
  boosts.

## Public API

| Function | Description |
|---|---|
| `usage_create(retention, &out)` / `usage_destroy()` | Empty owned summary; entries stop counting after `retention` seconds. |
| `usage_record(usage, target, query, ts)` | Count one open, from an optional typed query. Out of order is fine. |
| `usage_clear()` | Forget everything (history clearing). |
| `usage_merge(into, from)` | Add another summary, as if its opens were recorded here. |
| `usage_version()` / `usage_count()` / `usage_target()` | Item indices and their keys; the version changes whenever indices do. |
| `usage_boosts(usage, query, now, &boosts)` | One boost per item index for this query. |

Desktop ids and queries are copied. `usage_boosts` returns borrowed values,
valid until the next call or change. Single owner, no locks.

## Design

**Weights.** Each item and pair keeps one decayed open count and the time it
was measured. Exponential decay is linear, so adding an open (or merging a
weight measured at another time) only decays the older value to the later
time: O(1), no event list. The half-life is 14 days. Each weight also keeps
its strength, `log2(value) + time / half-life`: decaying two weights to any
common time preserves their order, so comparing strengths compares weights
with no `exp2` and no choice of time, even for weights measured after the
open being recorded (timestamps may arrive out of order).

**Boosts.** A weight `w` maps to `max * w / (w + half)`: frecency up to
`USAGE_FRECENCY_BOOST_MAX` (400, half at weight 2), query history up to
`USAGE_QUERY_BOOST_MAX` (1200, half at weight 1). Frecency stays below the
500-point gap between initials (4500) and token prefixes (5000), so it
reorders matches of similar strength. Query history can cross about one
channel tier. Both together fit `LEXICAL_BOOST_MAX`, below the exact-name
tier (static assertions).

**Query history.** Queries are normalized like the lexical engine (NFC, case
folding), trimmed, whitespace collapsed, and stored up to 32 symbols. Pairs
live in fixed slots; a separate array of two-byte slot numbers keeps them
sorted by query, so the stored queries starting with a typed query are one
range found by binary search, and inserting or evicting a pair moves slot
numbers, not 176-byte pairs. Typed queries longer than 32 symbols get no
query boost.

**Caps and eviction.** 2048 items and 4096 pairs bound memory (about 1.3 MB)
and the per-keystroke side-pass work. A full table evicts its weakest entry,
by strength. Item ids and weights, and pair items and weights, are kept in
dense arrays so lookups and eviction scan a few kilobytes.
Evicting an item moves the last item into its index, renumbers that item's
pairs and changes the version. Entries whose last open is older than
retention give no boost and are evicted during the next refresh.

**Refresh.** Frecency boosts are cached and recomputed at most once a minute
or after a change. Each query copies them (O(items)) and adds the query
history range.

## Invariants

- Item indices `0..usage_count()` are dense; any index change bumps the version.
- The slot order is sorted by normalized query, and every used slot
  references a live item; free slots are exactly the unused ones.
- Boosts lie in `0..USAGE_FRECENCY_BOOST_MAX + USAGE_QUERY_BOOST_MAX`.

## Performance

At the caps (2048 items, 4096 pairs), `usage_boosts` costs p95 0.005 ms per
keystroke (`make bench`, 2026-10-08). Recording an open is O(items) to find
the key plus a sorted insert of a slot number; when a table is full, one
linear scan of strengths finds the entry to evict. The startup rebuild
replays every retained open: in the worst case (every open with a new query
and more targets than the cap), 20,000 opens take about 0.2 s at -O2 and
100,000 about 1.3 s, against 1.25 s and 7.9 s when eviction decayed every
weight with `exp2` and moved whole pairs (`make bench` reports
`usage_rebuild`).

## Testing

`test_usage.c` checks analytic boost values, the half-life, out-of-order
opens, retention eviction, query prefixes and normalization, capacity
eviction for both tables, eviction order with out-of-order timestamps,
clearing, and that merging equals recording everything in one summary.

## Gotchas

- Expiry shows within a minute, not instantly: cached frecency refreshes at
  most once a minute.
- A query of one character boosts every item opened from a query starting
  with it. That is intended (type `c`, get the app you always open).

## Related

- [personal](../personal/README.md), [lexical](../lexical/README.md), [writer](../writer/README.md)
- ADR: [0033](../../adr/0033-m5-personal-ranking.md)
