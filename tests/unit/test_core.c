/* Checked vector growth and arithmetic boundary tests. */
#include "test.h"
#include "torchlight/vec.h"
void test_core(void) {
    size_t bytes = 0;
    CHECK(tl_size_multiply(SIZE_MAX, 2, &bytes) == TL_LIMIT);
    CHECK(tl_size_multiply(0, SIZE_MAX, &bytes) == TL_OK && bytes == 0);
    CHECK(tl_size_multiply(1, 1, NULL) == TL_INVALID);
    tl_vec *vec = NULL;
    CHECK(vec_create(0, &vec) == TL_INVALID && vec == NULL);
    CHECK(vec_create(sizeof(size_t), &vec) == TL_OK);
    for (size_t i = 0; i < 1000; i++)
        CHECK(vec_append(vec, &i) == TL_OK);
    const size_t *items = vec_const_data(vec);
    CHECK(vec_count(vec) == 1000);
    for (size_t i = 0; i < 1000; i++)
        CHECK(items[i] == i);
    CHECK(vec_append(vec, NULL) == TL_INVALID);
    vec_destroy(vec);
    CHECK(vec_count(NULL) == 0);
    vec_destroy(NULL);
}
