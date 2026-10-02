# core

> **Status:** Implemented for M1: status codes, checked arithmetic, vectors, hash map, configuration
> **Source:** `src/core/common.c`, `src/core/vec.c`, `src/core/hashmap.c` · **Header:** `include/torchlight/common.h`, `include/torchlight/vec.h`, `include/torchlight/hashmap.h`
> **Tests:** `tests/unit/test_core.c`

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

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- Public headers document parameters, lifetimes and error contracts.
