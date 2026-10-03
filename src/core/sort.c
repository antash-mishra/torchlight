/* In-place heapsort avoids libc sort implementations' optional heap scratch. */
#include "torchlight/sort.h"

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
