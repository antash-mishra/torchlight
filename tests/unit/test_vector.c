/* Float cosine reference checked against independent long-double arithmetic. */
#include "test.h"
#include "torchlight/vector.h"
#include <float.h>
#include <math.h>
#include <pthread.h>

enum { VECTOR_FIXTURE_ROWS = 37, VECTOR_FIXTURE_DIMENSIONS = 5 };

static long double reference_cosine(const float *left, const float *right, size_t dimensions) {
    long double dot = 0, left_norm = 0, right_norm = 0;
    for (size_t i = 0; i < dimensions; i++) {
        dot += (long double)left[i] * right[i];
        left_norm += (long double)left[i] * left[i];
        right_norm += (long double)right[i] * right[i];
    }
    return dot / sqrtl(left_norm * right_norm);
}

/* Insertion ordering deliberately differs from the implementation's heap. */
static void reference_order(const float values[VECTOR_FIXTURE_ROWS][VECTOR_FIXTURE_DIMENSIONS],
                            const float *query, tl_vector_result *results) {
    for (size_t i = 0; i < VECTOR_FIXTURE_ROWS; i++) {
        tl_vector_result result = {
            i + 1, (double)reference_cosine(values[i], query, VECTOR_FIXTURE_DIMENSIONS)};
        size_t position = i;
        while (position > 0 && results[position - 1].cosine < result.cosine) {
            results[position] = results[position - 1];
            position--;
        }
        results[position] = result;
    }
}

static void check_reference(void) {
    float values[VECTOR_FIXTURE_ROWS][VECTOR_FIXTURE_DIMENSIONS];
    const float query[] = {0.7F, -1.2F, 0.3F, 0.9F, -0.5F};
    tl_vector *index = NULL;
    tl_vector_workspace *workspace = NULL;
    CHECK(vector_create(7, VECTOR_FIXTURE_DIMENSIONS, VECTOR_FIXTURE_ROWS, SIZE_MAX, &index) ==
          TL_OK);
    for (size_t i = 0; i < VECTOR_FIXTURE_ROWS; i++) {
        for (size_t j = 0; j < VECTOR_FIXTURE_DIMENSIONS; j++)
            values[i][j] = (float)((int)((i * 19 + j * 13 + i * j * 7) % 61) - 30);
        CHECK(vector_add(index, i + 1, 7, values[i], VECTOR_FIXTURE_DIMENSIONS) == TL_OK);
    }
    CHECK(vector_count(index) == VECTOR_FIXTURE_ROWS && vector_emb_gen(index) == 7);
    CHECK(vector_finish(index) == TL_OK);
    CHECK(vector_workspace_create(index, &workspace) == TL_OK);
    tl_vector_result reference[VECTOR_FIXTURE_ROWS] = {0}, actual[VECTOR_FIXTURE_ROWS];
    reference_order((const float(*)[VECTOR_FIXTURE_DIMENSIONS])values, query, reference);
    for (size_t capacity = 1; capacity <= VECTOR_FIXTURE_ROWS; capacity++) {
        size_t count = 0;
        CHECK(vector_query(index, workspace, 7, query, VECTOR_FIXTURE_DIMENSIONS, actual, capacity,
                           &count) == TL_OK);
        CHECK(count == capacity);
        for (size_t i = 0; i < count; i++) {
            CHECK(actual[i].id == reference[i].id);
            CHECK(fabs(actual[i].cosine - reference[i].cosine) < 1e-6);
        }
    }
    vector_workspace_destroy(workspace);
    vector_destroy(index);
}

static void check_normalization(void) {
    float out[] = {8, 9}, values[] = {3, 4};
    CHECK(vector_normalize(values, 2, out) == TL_OK);
    CHECK(fabsf(out[0] - 0.6F) < 1e-6F && fabsf(out[1] - 0.8F) < 1e-6F);
    CHECK(vector_normalize(values, 2, values) == TL_OK);
    CHECK(values[0] == out[0] && values[1] == out[1]);
    const float invalid[][2] = {{0, 0}, {NAN, 1}, {1, INFINITY}, {-INFINITY, 1}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        CHECK(vector_normalize(invalid[i], 2, out) == TL_INVALID);
        CHECK(values[0] == out[0] && values[1] == out[1]);
    }
    const float large[] = {FLT_MAX, FLT_MAX}, small[] = {FLT_TRUE_MIN, FLT_TRUE_MIN};
    CHECK(vector_normalize(large, 2, out) == TL_OK);
    CHECK(fabsf(out[0] - 0.70710678F) < 1e-6F);
    CHECK(vector_normalize(small, 2, out) == TL_OK);
    CHECK(fabsf(out[0] - 0.70710678F) < 1e-6F);
    CHECK(vector_normalize(NULL, 2, out) == TL_INVALID);
    CHECK(vector_normalize(values, 0, out) == TL_INVALID);
    CHECK(vector_normalize(values, VECTOR_MAX_DIMENSIONS + 1, out) == TL_INVALID);
    CHECK(vector_normalize(values, 2, NULL) == TL_INVALID);
}

static void check_lifecycle(void) {
    tl_vector *index = NULL;
    tl_vector_workspace *workspace = NULL;
    float values[] = {2, 0};
    CHECK(vector_create(1, 2, 2, SIZE_MAX, &index) == TL_OK);
    CHECK(vector_workspace_create(index, &workspace) == TL_STATE && workspace == NULL);
    CHECK(vector_add(index, 1, 2, values, 2) == TL_STATE);
    CHECK(vector_add(index, 0, 1, values, 2) == TL_INVALID);
    CHECK(vector_add(index, 1, 1, values, 1) == TL_INVALID);
    const float zero[] = {0, 0};
    CHECK(vector_add(index, 1, 1, zero, 2) == TL_INVALID);
    CHECK(vector_count(index) == 0);
    CHECK(vector_add(index, 1, 1, values, 2) == TL_OK);
    values[0] = -2;
    CHECK(vector_add(index, 1, 1, values, 2) == TL_INVALID);
    CHECK(vector_add(index, 2, 1, values, 2) == TL_OK);
    CHECK(vector_add(index, 3, 1, values, 2) == TL_LIMIT);
    CHECK(vector_finish(index) == TL_OK);
    CHECK(vector_finish(index) == TL_STATE);
    CHECK(vector_add(index, 3, 1, values, 2) == TL_STATE);
    CHECK(vector_workspace_create(index, &workspace) == TL_OK);
    tl_vector_result results[2];
    size_t count = 99;
    CHECK(vector_query(index, workspace, 2, values, 2, results, 2, &count) == TL_STATE &&
          count == 0);
    CHECK(vector_query(index, workspace, 1, zero, 2, results, 2, &count) == TL_INVALID &&
          count == 0);
    CHECK(vector_query(index, workspace, 1, values, 2, results, 0, &count) == TL_INVALID);
    CHECK(vector_query(index, workspace, 1, values, 2, results, VECTOR_MAX_RESULTS + 1, &count) ==
          TL_INVALID);
    CHECK(vector_query(index, workspace, 1, values, 2, results, 2, &count) == TL_OK && count == 2);
    CHECK(results[0].id == 2 && results[0].cosine == 1);
    CHECK(results[1].id == 1 && results[1].cosine == -1);
    vector_workspace_destroy(workspace);
    vector_destroy(index);
}

static void check_empty_and_limits(void) {
    tl_vector *index = NULL, *other = NULL;
    tl_vector_workspace *workspace = NULL;
    const float query[] = {1, 0};
    CHECK(vector_create(0, 2, 0, SIZE_MAX, &index) == TL_INVALID && index == NULL);
    CHECK(vector_create(1, 0, 0, SIZE_MAX, &index) == TL_INVALID);
    CHECK(vector_create(1, VECTOR_MAX_DIMENSIONS + 1, 0, SIZE_MAX, &index) == TL_INVALID);
    CHECK(vector_create(1, 2, SIZE_MAX, SIZE_MAX, &index) == TL_LIMIT);
    CHECK(vector_create(1, 1, SIZE_MAX / (sizeof(float) + sizeof(uint64_t)), SIZE_MAX, &index) ==
          TL_LIMIT);
    CHECK(vector_create(1, 2, 0, 0, &index) == TL_LIMIT);
    CHECK(vector_create(1, 2, 0, SIZE_MAX, &index) == TL_OK);
    size_t bytes = vector_bytes(index);
    CHECK(vector_create(1, 2, 0, bytes - 1, &other) == TL_LIMIT);
    CHECK(vector_create(1, 2, 0, bytes, &other) == TL_OK);
    CHECK(vector_finish(index) == TL_OK && vector_finish(other) == TL_OK);
    CHECK(vector_workspace_create(index, &workspace) == TL_OK);
    tl_vector_result result;
    size_t count = 99;
    CHECK(vector_query(other, workspace, 1, query, 2, &result, 1, &count) == TL_INVALID);
    CHECK(vector_query(index, workspace, 1, query, 2, &result, 1, &count) == TL_OK && count == 0);
    vector_workspace_destroy(workspace);
    vector_destroy(other);
    vector_destroy(index);
    vector_destroy(NULL);
    vector_workspace_destroy(NULL);
    CHECK(vector_count(NULL) == 0 && vector_emb_gen(NULL) == 0 && vector_bytes(NULL) == 0);
}

struct query_thread {
    const tl_vector *index;
    tl_vector_workspace *workspace;
};

static void *run_queries(void *context) {
    const struct query_thread *thread = context;
    const float query[] = {1, 0};
    for (size_t i = 0; i < 500; i++) {
        tl_vector_result results[4];
        size_t count = 0;
        CHECK(vector_query(thread->index, thread->workspace, 3, query, 2, results, 4, &count) ==
              TL_OK);
        CHECK(count == 4 && results[0].id == 1 && results[1].id == 2 && results[2].id == 3);
        CHECK(results[3].id == 4 && results[3].cosine == 0);
    }
    return NULL;
}

static void check_ties_and_readers(void) {
    tl_vector *index = NULL;
    CHECK(vector_create(3, 2, 4, SIZE_MAX, &index) == TL_OK);
    const float values[][2] = {{1, 0}, {2, 0}, {3, 0}, {0, 1}};
    for (size_t i = 0; i < 4; i++)
        CHECK(vector_add(index, i + 1, 3, values[i], 2) == TL_OK);
    CHECK(vector_finish(index) == TL_OK);
    struct query_thread readers[2] = {{.index = index}, {.index = index}};
    pthread_t threads[2];
    for (size_t i = 0; i < 2; i++) {
        CHECK(vector_workspace_create(index, &readers[i].workspace) == TL_OK);
        CHECK(pthread_create(&threads[i], NULL, run_queries, &readers[i]) == 0);
    }
    for (size_t i = 0; i < 2; i++) {
        CHECK(pthread_join(threads[i], NULL) == 0);
        vector_workspace_destroy(readers[i].workspace);
    }
    vector_destroy(index);
}

static void check_int8(void) {
    tl_vector *index = NULL;
    CHECK(vector_create_int8(11, 5, 3, 1, &index) == TL_LIMIT && index == NULL);
    CHECK(vector_create_int8(11, 5, 3, SIZE_MAX, &index) == TL_OK);
    float values[] = {0.8F, -0.6F, 0, 0, 0};
    CHECK(vector_add(index, 1, 11, values, 5) == TL_OK);
    float other[] = {0, 0, 1, 0, 0};
    CHECK(vector_add(index, 2, 11, other, 5) == TL_OK);
    CHECK(vector_finish(index) == TL_OK);
    tl_vector_workspace *workspace = NULL;
    CHECK(vector_workspace_create(index, &workspace) == TL_OK);
    tl_vector_result results[2];
    size_t count = 0;
    CHECK(vector_query(index, workspace, 11, values, 5, results, 2, &count) == TL_OK);
    CHECK(count == 2 && results[0].id == 1 && results[1].id == 2);
    const float integer[] = {102, -76, 0, 0, 0};
    CHECK(fabs(results[0].cosine - (double)reference_cosine(integer, values, 5)) < 1e-6);
    CHECK(results[1].cosine == 0);
    CHECK(vector_query(index, workspace, 12, values, 5, results, 2, &count) == TL_STATE);
    CHECK(vector_bytes(index) < 256);
    vector_workspace_destroy(workspace);
    vector_destroy(index);
}

static void check_binary(void) {
    tl_vector *index = NULL;
    CHECK(vector_create_binary_int8(11, 5, 3, 0, SIZE_MAX, &index) == TL_INVALID);
    CHECK(vector_create_binary_int8(11, 5, 3, 2, SIZE_MAX, &index) == TL_OK);
    float first[] = {1, -1, 1, -1, 1}, second[] = {-1, 1, -1, 1, -1};
    CHECK(vector_add(index, 1, 11, first, 5) == TL_OK);
    CHECK(vector_add(index, 2, 11, second, 5) == TL_OK);
    CHECK(vector_add(index, 3, 11, first, 5) == TL_OK);
    CHECK(vector_finish(index) == TL_OK);
    tl_vector_workspace *workspace = NULL;
    CHECK(vector_workspace_create(index, &workspace) == TL_OK);
    tl_vector_result results[3];
    size_t count = 0;
    CHECK(vector_query(index, workspace, 11, first, 5, results, 2, &count) == TL_OK);
    CHECK(count == 2 && results[0].id == 1 && results[1].id == 3);
    CHECK(vector_query(index, workspace, 11, first, 5, results, 1, &count) == TL_OK);
    CHECK(count == 1 && results[0].id == 1);
    CHECK(vector_query(index, workspace, 11, first, 5, results, 3, &count) == TL_LIMIT);
    CHECK(vector_query(index, workspace, 11, second, 5, results, 2, &count) == TL_OK);
    CHECK(count == 2 && results[0].id == 2 && results[1].id == 1);
    vector_workspace_destroy(workspace);
    vector_destroy(index);
}

void test_vector(void) {
    check_binary();
    check_int8();
    check_reference();
    check_normalization();
    check_lifecycle();
    check_empty_and_limits();
    check_ties_and_readers();
}
