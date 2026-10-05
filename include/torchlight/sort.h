/* Allocation-free ordering of caller-owned arrays. */
#ifndef TORCHLIGHT_SORT_H
#define TORCHLIGHT_SORT_H
#include "torchlight/common.h"
/** Compare borrowed items: negative/zero/positive for before/equal/after.
 * context is borrowed for this call; the callback must not mutate items. */
typedef int (*tl_sort_compare)(const void *left, const void *right, const void *context);
/** Sort count fixed-size items in place by compare, using O(1) scratch and
 * O(count log count) comparisons without allocation or I/O. Arrays/context
 * remain caller-owned; equal items need not retain their original order.
 * TL_INVALID for zero item_size, NULL compare or missing nonempty items;
 * TL_LIMIT for count * item_size overflow. Errors leave items unchanged. */
tl_status sort_items(void *items, size_t count, size_t item_size, tl_sort_compare compare,
                     const void *context);
/** Sort count unsigned 64-bit integers ascending in place, retaining duplicates.
 * values is borrowed for this call; NULL is valid only when count is zero.
 * Uses O(1) scratch and O(count log count) work, without allocation or I/O.
 * Returns TL_INVALID for a missing nonempty array, otherwise TL_OK. */
tl_status sort_u64(uint64_t *values, size_t count);
#endif
