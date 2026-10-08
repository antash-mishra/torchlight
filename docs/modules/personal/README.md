# personal

> **Status:** Implemented (M5)
> **Source:** `src/service/personal.c` · **Header:** `include/torchlight/personal.h`
> **Tests:** `tests/unit/test_personal.c`, `tests/test_daemon.py`, `tests/test_desktop.py`

## Purpose

The daemon search thread's personal-ranking state
([ADR 0033](../../adr/0033-m5-personal-ranking.md)). It holds the live
[usage](../usage/README.md) summary and turns it into boosts for each query.
It also remembers which query each recent search came from, so an open can
record it.

## Responsibilities

- Owns the live usage summary, the last 32 searches (id and query) and the
  last 32 launch-event ids.
- Splits the summary into file ids for `catalog_query_boosted` and desktop
  positions for `desktop_query_boosted`.
- Adopts the startup summary the persistence thread rebuilt, merging opens
  recorded since startup, or discards it when history was cleared meanwhile.
- Does **not** save anything or run SQL: the daemon queues opens to the
  [writer](../writer/README.md), whose persistence thread writes them.
- Does **not** lock. Only the search thread calls it; `personal_items` is
  atomic for status reports from the IPC thread.

## Public API

| Function | Description |
|---|---|
| `personal_create(retention, &out)` / `personal_destroy()` | State with an empty live summary. |
| `personal_adopt(personal, loaded)` | Take ownership of the startup summary. |
| `personal_remember(personal, search_id, query)` / `personal_query_of()` | Recent searches, so an open can record its query. |
| `personal_record(personal, event_id, target, query, now)` | Count an accepted open once per launch-event id. |
| `personal_clear()` | History clearing. |
| `personal_boosts(personal, desktop, query, now, &files, &apps, &count)` | This query's catalog and desktop boosts. |
| `personal_items()` | Live item count, from any thread. |

## Design

Item indices are split by kind whenever the summary's version changes.
Application items are matched to the leased desktop snapshot's positions
whenever `desktop_gen` changes (a linear scan per refresh). Per query only the
boost values are copied into the prepared lists.

The catalog key for lease mappings is a counter that advances with every
split. A usage version cannot serve as the key: adopting the startup summary
replaces the summary object, and its version could repeat an old one.

## Data flow

```
search thread: query  -> personal_boosts -> catalog_query_boosted + desktop_query_boosted
               open   -> personal_query_of -> writer_history (queued) -> personal_record
               clear  -> personal_clear -> writer_history (queued)
               job    -> writer_take_usage -> personal_adopt (once)
persistence thread: store_history_opens -> usage summary -> offered to the search thread
```

## Invariants

- Every boost list refers to the summary as it is now: mappings are redone
  after any version change, adoption or clear.
- A startup summary is never adopted after a clear.

## Testing

`test_personal.c` covers search memory wraparound, retried events counting
once, file boosts and keys, merging on adoption, and discarding after a clear.
The daemon and desktop integration tests cover lifting a file or an application
among equal matches, persistence across a restart, and clearing.

## Gotchas

- An open records a query only if its search is among the last 32. Older opens
  still count for frecency.
- An open that the bounded history queue drops still counts in the live
  summary; it just won't survive a restart.

## Related

- [usage](../usage/README.md), [daemon](../daemon/README.md), [writer](../writer/README.md), [catalog](../catalog/README.md)
- ADR: [0033](../../adr/0033-m5-personal-ranking.md)
