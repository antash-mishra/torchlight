# core

> **Status:** Partially implemented: checked vector and status utilities
> **Source:** `src/core/common.c`, `src/core/vec.c` · **Header:** `include/torchlight/common.h`, `include/torchlight/vec.h`
> **Tests:** `tests/unit/test_core.c`

## Behavior and ownership

`tl_status` describes invalid input, allocation, I/O, lifecycle and resource-limit
errors. `tl_size_multiply` checks allocation arithmetic. `tl_vec` is an opaque
contiguous array with geometric growth, element-copy append and borrowed data
views. Owners destroy nested elements before the vector; successful growth
invalidates data pointers. Append sources must not alias vector storage.

Vector construction can allocate; queries do not grow vectors. The initial
query workspace uses fixed symbol buffers and arrays allocated before searching.
Arena/hashmap/log helpers remain planned until needed. There is no mutable global
state. Tests cover growth across capacity boundaries and arithmetic overflow.

## Related

- [Implementation increment ADR](../../adr/0006-m1-prefix-subsequence-baseline.md)
- [Evaluation](../../evaluation.md)
- Public headers document parameters, lifetimes and error contracts.
