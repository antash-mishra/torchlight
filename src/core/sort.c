/* In-place heapsort avoids libc sort implementations' optional heap scratch. */
#include "torchlight/sort.h"

static void swap_items(unsigned char *left, unsigned char *right, size_t item_size) {
    for (size_t i = 0; i < item_size; i++) {
        unsigned char byte = left[i];
        left[i] = right[i];
        right[i] = byte;
    }
}

static void sift_items(unsigned char *items, size_t position, size_t count, size_t item_size,
                       tl_sort_compare compare, const void *context) {
    while (position < count / 2) {
        size_t child = position * 2 + 1;
        if (child + 1 < count &&
            compare(items + child * item_size, items + (child + 1) * item_size, context) < 0)
            child++;
        if (compare(items + position * item_size, items + child * item_size, context) >= 0)
            return;
        swap_items(items + position * item_size, items + child * item_size, item_size);
        position = child;
    }
}

tl_status sort_items(void *items, size_t count, size_t item_size, tl_sort_compare compare,
                     const void *context) {
    if (item_size == 0 || compare == NULL || (items == NULL && count != 0))
        return TL_INVALID;
    size_t bytes = 0;
    tl_status status = tl_size_multiply(count, item_size, &bytes);
    if (status != TL_OK || count < 2)
        return status;
    unsigned char *data = items;
    for (size_t parent = count / 2; parent > 0; parent--)
        sift_items(data, parent - 1, count, item_size, compare, context);
    for (size_t remaining = count; remaining > 1; remaining--) {
        swap_items(data, data + (remaining - 1) * item_size, item_size);
        sift_items(data, 0, remaining - 1, item_size, compare, context);
    }
    return TL_OK;
}

static void sift_down(uint64_t *values, size_t position, size_t count) {
    uint64_t value = values[position];
    while (position < count / 2) {
        size_t child = position * 2 + 1;
        if (child + 1 < count && values[child] < values[child + 1])
            child++;
        if (value >= values[child])
            break;
        values[position] = values[child];
        position = child;
    }
    values[position] = value;
}

tl_status sort_u64(uint64_t *values, size_t count) {
    if (values == NULL && count != 0)
        return TL_INVALID;
    if (count < 2)
        return TL_OK;
    for (size_t parent = count / 2; parent > 0; parent--)
        sift_down(values, parent - 1, count);
    for (size_t remaining = count; remaining > 1; remaining--) {
        uint64_t largest = values[0];
        values[0] = values[remaining - 1];
        values[remaining - 1] = largest;
        sift_down(values, 0, remaining - 1);
    }
    return TL_OK;
}
