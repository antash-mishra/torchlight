# place

> **Status:** Implemented (M8): depth and dependency-tree penalties for file ranking
> **Source:** `src/index/place.c` · **Header:** `include/torchlight/place.h`
> **Tests:** `tests/unit/test_place.c`, `tests/unit/test_lexical_place.c`

## Purpose
A query-independent prior for file ranking: how likely a file's location
holds what a person searches for. Files deep below the indexed folder, or
inside a tree of installed or vendored software, get a small penalty, so the
person's own files lead among matches of similar strength. See
[ADR 0047](../../adr/0047-place-ranking-and-excluded-folders.md).

## Responsibilities
- Recognize dependency-tree markers from a normalized directory name.
- Carry a directory's place (level, inside a dependency tree) to its children.
- Turn a parent directory's place into a capped penalty.
- It does **not** store anything per engine or apply penalties to scores:
  [lexical](../lexical/README.md) computes one penalty per interned directory
  at seal time, stores it per entry and subtracts it during queries.

## Public API
| Function | Description |
|---|---|
| `place_dependency_name(name)` | Whether a normalized directory name marks a dependency or toolchain tree. |
| `place_child(parent, name, usable)` | The place of a directory directly below `parent`. |
| `place_penalty(parent)` | Penalty, 0 to `PLACE_PENALTY_MAX`, for an entry inside `parent`. |

Pure functions over borrowed names: no allocation, I/O, ownership or errors.
This module has no opaque type because it keeps no state; a policy object can
come later if markers become configurable.

## Design
- **Level.** The indexed root's own folder is level 1; directories above
  every root (`/home` for root `~`) are not *usable* and add nothing, as for
  parent matching. An entry's depth is its parent's level minus one:
  `~/notes.txt` is at depth 0, `~/workspace/torchlight/scripts` at depth 2.
- **Dependency markers.** `node_modules`, `bower_components`,
  `site-packages`, `dist-packages`, `vendor`, `third_party`, `third-party`,
  `thirdparty`, `sysroot`, `sdk` (compared case-folded); Go module folders
  (contain `@v` followed by a digit, or are exactly `@v`); versioned install
  folders (`v?D{1,3}(.D+)+`, such as `27.0.12077973` or `1.4.335.0`). The lead
  is limited to three digits so dotted dates (`2024.05.01`) do not match. A
  marker's descendants are in the tree; the marker folder itself is not. Since
  ADR 0062 the crawler leaves out `node_modules`, `bower_components`,
  `site-packages`, `dist-packages` and every folder a tool marks as its own, so
  the penalty matters for the rest (and for those an `allow` brings back).
- **Penalty.** `PLACE_DEPTH_PENALTY` (24) per directory beyond
  `PLACE_FREE_DEPTH` (3), plus `PLACE_DEPENDENCY_PENALTY` (128) inside a
  dependency tree, capped at `PLACE_PENALTY_MAX` (256). Three extra levels, or
  a dependency tree alone, outweigh the whole 64-point short-name bonus. The
  cap stays below the smallest gap between lexical channel tiers (500, from
  initials to token prefix), checked by a static assertion in `lexical`.

The weights were chosen on the live catalog (see the
[M8 plan](../../m8-plan.md)): caps from 256 to 600 gave nearly the same
top-10 results, so the smallest was kept.

## Invariants
- `place_penalty` is in `0..PLACE_PENALTY_MAX` for every input, including
  saturated levels.
- A place depends only on the names and usable flags along the path, so the
  same path gets the same penalty in any engine with the same roots (base and
  delta segments).

## Testing
`test_place.c` covers every marker, case folding, Go module and cache names,
version folders and near misses (`2024.05.01`, `v1`, `1.`, `.1`, `1..2`,
`sdkfoo`, `python3.11`), unusable directories, inheritance below a marker,
level saturation and the cap. `test_lexical_place.c` covers the engine
integration.

## Gotchas
- `sdk` also matches a person's own SDK project folder; its contents rank a
  little lower until opening them adds personal boosts.
- The marker list is policy, not a standard. Other ecosystems (for example
  `.m2` or Ruby gem folders) are hidden folders and not crawled by default.

## Related
- Modules: [lexical](../lexical/README.md), [crawl](../crawl/README.md)
  (`exclude` removes a tree entirely)
- ADRs: [0047](../../adr/0047-place-ranking-and-excluded-folders.md),
  [0033](../../adr/0033-m5-personal-ranking.md) (the same tier-gap rule for
  frecency)
