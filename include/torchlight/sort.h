/* Allocation-free ordering of caller-owned integer arrays. */
#ifndef TORCHLIGHT_SORT_H
#define TORCHLIGHT_SORT_H
#include "torchlight/common.h"
/** Sort count unsigned 64-bit integers ascending in place, retaining duplicates.
 * values is borrowed for this call; NULL is valid only when count is zero.
 * Uses O(1) scratch and O(count log count) work, without allocation or I/O.
 * Returns TL_INVALID for a missing nonempty array, otherwise TL_OK. */
tl_status sort_u64(uint64_t *values, size_t count);
#endif
