/* Bitmap candidates equal a scalar mask scan, including partial final words. */
#include "test.h"
#include "torchlight/mask.h"

static void check_query(const tl_mask_index *index, tl_mask_scratch *scratch, const uint64_t *masks,
                        size_t count, uint64_t required) {
    CHECK(mask_index_query(index, scratch, required) == TL_OK);
    size_t expected = 0;
    for (size_t row = 0; row < count; row++)
        expected += (masks[row] & required) == required;
    CHECK(mask_index_count(scratch) == expected);
    for (size_t row = 0; row < count; row++) {
        if ((masks[row] & required) != required)
            continue;
        size_t actual = SIZE_MAX;
        CHECK(mask_index_next(scratch, &actual) && actual == row);
    }
    size_t actual = SIZE_MAX;
    CHECK(!mask_index_next(scratch, &actual) && actual == SIZE_MAX);
    CHECK(mask_index_count(scratch) == 0);
}
void test_mask(void) {
    tl_mask_index *index = NULL;
    tl_mask_scratch *scratch = NULL;
    CHECK(mask_index_create(NULL, 1, &index) == TL_INVALID && index == NULL);
    CHECK(mask_index_create(NULL, SIZE_MAX, &index) == TL_INVALID);
    CHECK(mask_index_create(NULL, 0, &index) == TL_OK);
    CHECK(mask_scratch_create(index, &scratch) == TL_OK);
    check_query(index, scratch, NULL, 0, 0);
    CHECK(mask_index_include(scratch, 0) == TL_INVALID);
    mask_scratch_destroy(scratch);
    mask_index_destroy(index);
    uint64_t masks[131];
    for (size_t row = 0; row < 131; row++)
        masks[row] = row == 0 ? UINT64_MAX : UINT64_C(1) << (row % 64) | row;
    CHECK(mask_index_create(masks, SIZE_MAX, &index) == TL_LIMIT && index == NULL);
    CHECK(mask_index_create(masks, 131, &index) == TL_OK);
    CHECK(mask_scratch_create(index, &scratch) == TL_OK);
    for (size_t bit = 0; bit < 64; bit++) {
        check_query(index, scratch, masks, 131, UINT64_C(1) << bit);
        check_query(index, scratch, masks, 131, (UINT64_C(1) << bit) | 3);
    }
    check_query(index, scratch, masks, 131, UINT64_MAX);
    check_query(index, scratch, masks, 131, 0);
    CHECK(mask_index_query(index, scratch, UINT64_MAX) == TL_OK);
    CHECK(mask_index_include(scratch, 130) == TL_OK);
    CHECK(mask_index_include(scratch, 130) == TL_OK);
    size_t actual = SIZE_MAX;
    CHECK(mask_index_next(scratch, &actual) && actual == 0);
    CHECK(mask_index_next(scratch, &actual) && actual == 130);
    CHECK(!mask_index_next(scratch, &actual));
    mask_scratch_destroy(scratch);
    mask_index_destroy(index);
}
