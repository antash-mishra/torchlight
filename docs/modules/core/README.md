# core

> **Status:** Implemented: status/arithmetic, vectors, hash map, configuration, JSON/base64, byte scopes, in-place sort, mask bitmaps and bounded workers
> **Source:** `src/core/` · **Headers:** `include/torchlight/{common,vec,hashmap,json,path,sort,mask,parallel}.h`
> **Tests:** `tests/unit/test_{core,json,sort,mask,parallel}.c`

## Behavior and ownership

`tl_status` describes invalid input, allocation, I/O, lifecycle and resource-limit
errors. `tl_size_multiply` checks allocation arithmetic.

`tl_vec` is an opaque contiguous array with checked geometric growth. It
supports single and bulk appends, `vec_reserve`, `vec_clear` and `vec_shrink`
(release slack after construction). Growth invalidates borrowed element
pointers; failed growth leaves storage unchanged.

`tl_hashmap` is an open-addressing map from caller-computed 64-bit hashes to
`uint32_t` values (typically indexes into the caller's own arrays). Keys stay
with the caller: lookups pass an equality callback, so one map type serves any
key without copying it. Linear probing keeps the load at or below one half.
`hashmap_hash` is FNV-1a with an extendable state (hashing pieces equals hashing
their concatenation); buckets apply a final mix to its weak low bits.

The [configuration](../config/README.md) resolver also lives in `src/core/`.
Tests cover overflow, vector growth/bulk/clear/shrink, map growth, equal hashes
with different keys, misses and piecewise hashing.

`json.h` / `src/core/json.c` parse into caller-owned token buffers and encode into
caller-owned output buffers without allocation. Strings validate UTF-8 with the
existing utf8proc library; Unicode escapes pair surrogates and reject decoded
NUL. Depth is bounded to eight, object keys to 127 decoded bytes, and duplicate
keys are rejected even when spelled with escapes. Number syntax is strict;
unsigned extraction rejects overflow. Base64 preserves non-NUL raw bytes and
rejects noncanonical encodings. Tests cover malformed/duplicate JSON, Unicode,
capacity exhaustion and round trips of all 255 non-NUL byte values.

`path.h` / `src/core/path.c` provide component-aware byte scope checks without
filesystem I/O. These stateless buffer/value utilities need no mutable context.

`sort_items` orders caller-owned fixed-size records with a context-bearing
comparison callback, checked size arithmetic and in-place heapsort. M4 uses it
for fusion grouping/order and vector result ordering without query allocation.
Tests cover record payload preservation, both directions, ties, all small heap
sizes and overflow. The optimized integer implementation remains unchanged.

`sort_u64` orders caller-owned integers with an in-place heapsort, constant
scratch and no allocation. Trigram deduplication uses it because libc `qsort`
can allocate temporary storage even when the caller supplies a fixed array.

`tl_mask_index` is an immutable generic index of 64-bit row masks: one resident
bitmap per mask bit. Queries intersect required-bit postings into owned scratch,
can union additional rows, and iterate/count the complete set. At 500k rows the
index uses about 4 MB; scratch uses about 63 kB. A scalar-scan oracle covers every
bit, empty input, partial bitmap words, inclusion/deduplication and overflow.

`tl_parallel` owns a bounded pthread pool, including its coordinator. Threads
start during creation, sleep between runs, and join during destruction. Each
dispatch partitions a borrowed context's indexes into complete disjoint ranges;
it performs no allocation and waits for every participant even on an error.
Callbacks receive their participant index (0 for the caller, stable per
worker), so callers can keep per-thread scratch such as compiled fuzzy
matchers. Tests cover all supported pool sizes, empty/tiny/uneven ranges,
contiguous participant ownership, repeated runs and recovery after callback
failure. It uses the existing pthread dependency.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Query contract and scan improvements](../../adr/0013-query-contracts-and-resident-filters.md)
- Public headers document parameters, lifetimes and error contracts.
