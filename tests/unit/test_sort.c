/* Integer ordering handles duplicates, extremes and empty caller buffers. */
#include "test.h"
#include "torchlight/sort.h"
#include <string.h>

void test_sort(void) {
    uint64_t values[] = {UINT64_MAX, 1, 0, 9, 1, UINT64_MAX, 3};
    const uint64_t expected[] = {0, 1, 1, 3, 9, UINT64_MAX, UINT64_MAX};
    CHECK(sort_u64(NULL, 0) == TL_OK);
    CHECK(sort_u64(NULL, 1) == TL_INVALID);
    CHECK(sort_u64(values, 1) == TL_OK && values[0] == UINT64_MAX);
    CHECK(sort_u64(values, sizeof(values) / sizeof(values[0])) == TL_OK);
    CHECK(memcmp(values, expected, sizeof(values)) == 0);
    uint64_t reverse[1024];
    for (size_t i = 0; i < 1024; i++)
        reverse[i] = 1024 - i;
    CHECK(sort_u64(reverse, 1024) == TL_OK);
    for (size_t i = 0; i < 1024; i++)
        CHECK(reverse[i] == i + 1);
}
