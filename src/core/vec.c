/* Checked geometric growth for reusable contiguous arrays. */
#include "torchlight/vec.h"
#include <stdlib.h>
#include <string.h>
struct tl_vec {
    void *data;
    size_t count, capacity, element_size;
};
tl_status vec_create(size_t element_size, tl_vec **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (element_size == 0)
        return TL_INVALID;
    tl_vec *vec = calloc(1, sizeof(*vec));
    if (vec == NULL)
        return TL_NOMEM;
    vec->element_size = element_size;
    *out = vec;
    return TL_OK;
}
void vec_destroy(tl_vec *vec) {
    if (vec == NULL)
        return;
    free(vec->data);
    free(vec);
}
static tl_status resize(tl_vec *vec, size_t capacity) {
    size_t bytes = 0;
    tl_status status = tl_size_multiply(capacity, vec->element_size, &bytes);
    if (status != TL_OK)
        return status;
    void *data = realloc(vec->data, bytes);
    if (data == NULL)
        return TL_NOMEM;
    vec->data = data;
    vec->capacity = capacity;
    return TL_OK;
}
/* Geometric growth keeps appends amortized O(1). */
static tl_status grow_to(tl_vec *vec, size_t needed) {
    const size_t initial_capacity = 16;
    if (needed <= vec->capacity)
        return TL_OK;
    size_t capacity = vec->capacity == 0 ? initial_capacity : vec->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return TL_LIMIT;
        capacity *= 2;
    }
    return resize(vec, capacity);
}
tl_status vec_append(tl_vec *vec, const void *item) {
    return vec_append_array(vec, item, 1);
}
tl_status vec_append_array(tl_vec *vec, const void *items, size_t count) {
    if (vec == NULL || (items == NULL && count != 0))
        return TL_INVALID;
    if (count == 0)
        return TL_OK;
    if (count > SIZE_MAX - vec->count)
        return TL_LIMIT;
    tl_status status = grow_to(vec, vec->count + count);
    if (status != TL_OK)
        return status;
    memcpy((unsigned char *)vec->data + vec->count * vec->element_size, items,
           count * vec->element_size);
    vec->count += count;
    return TL_OK;
}
tl_status vec_reserve(tl_vec *vec, size_t capacity) {
    if (vec == NULL)
        return TL_INVALID;
    return capacity <= vec->capacity ? TL_OK : resize(vec, capacity);
}
void vec_clear(tl_vec *vec) {
    if (vec != NULL)
        vec->count = 0;
}
void vec_truncate(tl_vec *vec, size_t count) {
    if (vec != NULL && count < vec->count)
        vec->count = count;
}
void vec_shrink(tl_vec *vec) {
    if (vec == NULL || vec->count == vec->capacity || vec->count == 0)
        return;
    tl_status status = resize(vec, vec->count);
    (void)status; /* keeping the larger block on failure is harmless */
}
void *vec_data(tl_vec *vec) {
    return vec == NULL ? NULL : vec->data;
}
const void *vec_const_data(const tl_vec *vec) {
    return vec == NULL ? NULL : vec->data;
}
size_t vec_count(const tl_vec *vec) {
    return vec == NULL ? 0 : vec->count;
}
