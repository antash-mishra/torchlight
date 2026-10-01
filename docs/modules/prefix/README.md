# prefix

> **Status:** Implemented for first M1 increment
> **Source:** `src/index/prefix.c` · **Header:** `include/torchlight/prefix.h`
> **Tests:** `tests/unit/test_prefix.c`

## Behavior and ownership

The opaque prefix index sorts borrowed normalized basename/token slices by
symbol order and entry slot. It additionally owns a compact basename-initials
key (`projectNotes.md` -> `pnm`). `prefix_finish` seals construction; query
binary-searches the lower bound and expands all matching keys without truncating
by file id. Slots address caller score arrays, deduplicating multiple key hits.

Key priorities favor full basename prefixes, then basename tokens/initials,
then parent tokens. The engine retains normalized arrays for the index lifetime;
key pointers never refer into a relocating entry vector. Empty indexes are valid.

Tests cover build lifecycle, insufficient score buffers, short prefixes,
initials and basename-over-parent priority. Build sorting is O(K log K); lookup
is O(log K + expanded postings), with all postings represented initially as
sorted key/slot records. Exact raw-path lookup remains a separate engine scan.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
