/* Generic contiguous arrays; growth occurs only during construction. */
#ifndef TORCHLIGHT_VEC_H
#define TORCHLIGHT_VEC_H
#include "torchlight/common.h"
typedef struct tl_vec tl_vec;
/** Create an owned vector of nonzero element_size; out is NULL on failure. */
tl_status vec_create(size_t element_size, tl_vec **out);
/** Free vector storage; NULL is allowed. Nested pointees remain caller-owned. */
void vec_destroy(tl_vec *vec);
/** Append a copy of item; invalid arguments/overflow/allocation return status.
 * item must not point into the vector. Successful growth invalidates borrowed
 * element pointers; failed growth leaves storage unchanged. */
tl_status vec_append(tl_vec *vec, const void *item);
/** Append count copies from items (which must not point into the vector).
 * count zero is a no-op. Same growth, error and invalidation rules as append. */
tl_status vec_append_array(tl_vec *vec, const void *items, size_t count);
/** Ensure room for at least capacity elements without further growth. Errors:
 * TL_INVALID/TL_LIMIT/TL_NOMEM, leaving storage unchanged. */
tl_status vec_reserve(tl_vec *vec, size_t capacity);
/** Remove all elements, keeping capacity; NULL is allowed, no errors. */
void vec_clear(tl_vec *vec);
/** Release unused capacity after construction. A failed shrink keeps the
 * larger block, so this cannot fail; borrowed element pointers are invalidated. */
void vec_shrink(tl_vec *vec);
/** Borrow contiguous storage until next append/destruction; NULL for NULL vec. */
void *vec_data(tl_vec *vec);
/** Borrow immutable storage for the same lifetime; NULL for NULL vec. */
const void *vec_const_data(const tl_vec *vec);
/** Return element count (zero for NULL); cannot fail. */
size_t vec_count(const tl_vec *vec);
#endif
