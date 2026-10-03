/* Immutable bitmap lookup for rows whose 64-bit masks contain required bits. */
#ifndef TORCHLIGHT_MASK_H
#define TORCHLIGHT_MASK_H
#include "torchlight/common.h"
#include <stdbool.h>

typedef struct tl_mask_index tl_mask_index;
typedef struct tl_mask_scratch tl_mask_scratch;

/** Build an owned immutable index from count borrowed row masks. Copies their
 * bits; masks may be NULL only when count is zero. out is NULL on failure.
 * Returns TL_INVALID, TL_LIMIT for size overflow, TL_NOMEM or TL_OK. */
tl_status mask_index_create(const uint64_t *masks, size_t count, tl_mask_index **out);
/** Free an index after destroying its scratch objects; NULL is allowed. */
void mask_index_destroy(tl_mask_index *index);
/** Allocate owned reusable query scratch for index, which must outlive it.
 * out is NULL on failure; returns TL_INVALID, TL_NOMEM or TL_OK. */
tl_status mask_scratch_create(const tl_mask_index *index, tl_mask_scratch **out);
/** Free query scratch; NULL is allowed. No errors. */
void mask_scratch_destroy(tl_mask_scratch *scratch);
/** Reset scratch to exactly the rows with (row_mask & required) == required.
 * No allocation or I/O; index is borrowed and separate scratch objects permit
 * concurrent queries. Returns TL_INVALID for NULL/mismatched scratch, else TL_OK. */
tl_status mask_index_query(const tl_mask_index *index, tl_mask_scratch *scratch, uint64_t required);
/** Include row in the pending result set, before its first mask_index_next call.
 * No allocation or I/O; TL_INVALID for NULL/out-of-range row, otherwise TL_OK. */
tl_status mask_index_include(tl_mask_scratch *scratch, size_t row);
/** Count pending rows without advancing the iterator. NULL returns zero.
 * No allocation or I/O; consumed rows are excluded from this count. */
size_t mask_index_count(const tl_mask_scratch *scratch);
/** Write the next included row to out, ascending without duplicates. Returns
 * false at exhaustion or for NULL arguments; out is unchanged then. Scratch
 * owns the iterator state; no allocation or I/O. */
bool mask_index_next(tl_mask_scratch *scratch, size_t *out);
#endif
