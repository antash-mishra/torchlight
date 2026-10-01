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
static tl_status grow(tl_vec *vec) {
    const size_t initial_capacity = 16;
    size_t capacity = vec->capacity == 0 ? initial_capacity : vec->capacity;
    if (vec->capacity != 0) {
        if (capacity > SIZE_MAX / 2)
            return TL_LIMIT;
        capacity *= 2;
    }
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
tl_status vec_append(tl_vec *vec, const void *item) {
    if (vec == NULL || item == NULL)
        return TL_INVALID;
    if (vec->count == vec->capacity) {
        tl_status status = grow(vec);
        if (status != TL_OK)
            return status;
    }
    memcpy((unsigned char *)vec->data + vec->count * vec->element_size, item, vec->element_size);
    vec->count++;
    return TL_OK;
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
