/* Allocation-free ordering handles generic records, duplicates and extremes. */
#include "test.h"
#include "torchlight/sort.h"
#include <string.h>

struct sort_record {
    uint64_t key, payload;
};

static int compare_records(const void *left, const void *right, const void *context) {
    const struct sort_record *a = left, *b = right;
    int direction = *(const int *)context;
    return a->key < b->key ? -direction : a->key > b->key ? direction : 0;
}

static void check_records(void) {
    const int ascending = 1, descending = -1;
    struct sort_record records[129];
    CHECK(sort_items(NULL, 0, sizeof(*records), compare_records, &ascending) == TL_OK);
    CHECK(sort_items(NULL, 1, sizeof(*records), compare_records, &ascending) == TL_INVALID);
    CHECK(sort_items(records, 1, 0, compare_records, &ascending) == TL_INVALID);
    CHECK(sort_items(records, 1, sizeof(*records), NULL, &ascending) == TL_INVALID);
    CHECK(sort_items(records, SIZE_MAX, sizeof(*records), compare_records, &ascending) == TL_LIMIT);
    for (size_t count = 0; count <= 129; count++) {
        for (size_t i = 0; i < count; i++)
            records[i] = (struct sort_record){count - i, (count - i) * 17};
        CHECK(sort_items(records, count, sizeof(*records), compare_records, &ascending) == TL_OK);
        for (size_t i = 0; i < count; i++)
            CHECK(records[i].key == i + 1 && records[i].payload == (i + 1) * 17);
        CHECK(sort_items(records, count, sizeof(*records), compare_records, &descending) == TL_OK);
        for (size_t i = 0; i < count; i++)
            CHECK(records[i].key == count - i && records[i].payload == (count - i) * 17);
    }
    struct sort_record ties[] = {{9, 153}, {1, 17}, {9, 153}, {1, 17}, {9, 153}};
    CHECK(sort_items(ties, 5, sizeof(*ties), compare_records, &ascending) == TL_OK);
    CHECK(ties[0].key == 1 && ties[1].key == 1 && ties[2].key == 9 && ties[4].key == 9);
}

void test_sort(void) {
    check_records();
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
