/* Float cosine reference checked against independent long-double arithmetic;
 * int8, prefix-shortlist and binary formats checked against analytic cases. */
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

/* Copied rows score bit-identically to their source rows (float, int8 and
 * binary formats), interleave with new rows by id, and reject mismatches. */
static void check_row_copy(void) {
    float first[] = {1, -1, 1, -1, 1}, second[] = {0.3F, 0.1F, -0.9F, 0, 0.2F};
    for (int format = 0; format < 3; format++) {
        tl_vector *source = NULL, *copy = NULL;
        tl_status created = format == 0 ? vector_create(11, 5, 3, SIZE_MAX, &source)
                            : format == 1
                                ? vector_create_int8(11, 5, 3, SIZE_MAX, &source)
                                : vector_create_binary_int8(11, 5, 3, 3, SIZE_MAX, &source);
        CHECK(created == TL_OK);
        CHECK(vector_add(source, 2, 11, first, 5) == TL_OK &&
              vector_add(source, 5, 11, second, 5) == TL_OK);
        CHECK(vector_finish(source) == TL_OK);
        created = format == 0   ? vector_create(11, 5, 3, SIZE_MAX, &copy)
                  : format == 1 ? vector_create_int8(11, 5, 3, SIZE_MAX, &copy)
                                : vector_create_binary_int8(11, 5, 3, 3, SIZE_MAX, &copy);
        CHECK(created == TL_OK);
        size_t position = 9;
        CHECK(vector_position(source, 5, &position) == TL_OK && position == 1);
        CHECK(vector_position(source, 3, &position) == TL_STATE && position == 1);
        CHECK(vector_add_row(copy, source, 0) == TL_OK);
        CHECK(vector_add(copy, 3, 11, second, 5) == TL_OK);   /* a new row between copies */
        CHECK(vector_add_row(copy, source, 0) == TL_INVALID); /* ids must increase */
        CHECK(vector_add_row(copy, source, 2) == TL_INVALID);
        CHECK(vector_add_row(copy, source, 1) == TL_OK && vector_finish(copy) == TL_OK);
        CHECK(vector_add_row(copy, source, 1) == TL_STATE);
        tl_vector_workspace *a = NULL, *b = NULL;
        CHECK(vector_workspace_create(source, &a) == TL_OK &&
              vector_workspace_create(copy, &b) == TL_OK);
        tl_vector_result from_source[2], from_copy[3];
        size_t na = 0, nb = 0;
        CHECK(vector_query(source, a, 11, second, 5, from_source, 2, &na) == TL_OK);
        CHECK(vector_query(copy, b, 11, second, 5, from_copy, 3, &nb) == TL_OK && nb == 3);
        for (size_t i = 0; i < na; i++) {
            size_t j = 0;
            while (j < nb && from_copy[j].id != from_source[i].id)
                j++;
            CHECK(j < nb && from_copy[j].cosine == from_source[i].cosine);
        }
        /* Excluding position 1 (id 3) hides that row until the bitmap is cleared. */
        const uint64_t excluded[] = {UINT64_C(1) << 1};
        vector_workspace_exclude(b, excluded);
        CHECK(vector_query(copy, b, 11, second, 5, from_copy, 3, &nb) == TL_OK && nb == 2);
        CHECK(from_copy[0].id != 3 && from_copy[1].id != 3);
        vector_workspace_exclude(b, NULL);
        CHECK(vector_query(copy, b, 11, second, 5, from_copy, 3, &nb) == TL_OK && nb == 3);
        vector_workspace_destroy(a);
        vector_workspace_destroy(b);
        vector_destroy(copy);
        vector_destroy(source);
    }
    tl_vector *floats = NULL, *compact = NULL;
    float values[] = {1, 0, 0, 0, 0};
    CHECK(vector_create(11, 5, 1, SIZE_MAX, &floats) == TL_OK &&
          vector_add(floats, 1, 11, values, 5) == TL_OK);
    CHECK(vector_create_int8(11, 5, 1, SIZE_MAX, &compact) == TL_OK);
    CHECK(vector_add_row(compact, floats, 0) == TL_INVALID); /* storage formats differ */
    vector_destroy(floats);
    vector_destroy(compact);
}
/* ---- prefix shortlist ------------------------------------------------------ */

enum {
    PREFIX_ROWS = 40,
    PREFIX_WIDE = 38, /* a 22-component tail ends with a two-component remainder */
    PREFIX_HEAD = 16,
    PREFIX_NARROW = 32
};

static void prefix_row(size_t row, float *out) {
    for (size_t j = 0; j < PREFIX_WIDE; j++)
        out[j] = (float)((int)((row * 29 + j * 17 + row * j * 5) % 71) - 35);
}

static tl_vector_workspace *sealed(tl_vector *index) {
    tl_vector_workspace *workspace = NULL;
    CHECK(vector_finish(index) == TL_OK && vector_workspace_create(index, &workspace) == TL_OK);
    return workspace;
}

/* Rescored cosines are bit-identical to vector_create_int8, and a shortlist
 * covering every row returns exactly its results at every output capacity. */
static void check_prefix_matches_int8(void) {
    tl_vector *exact = NULL, *covering = NULL, *narrow = NULL;
    CHECK(vector_create_int8(5, PREFIX_WIDE, PREFIX_ROWS, SIZE_MAX, &exact) == TL_OK);
    CHECK(vector_create_prefix_int8(5, PREFIX_WIDE, PREFIX_ROWS, PREFIX_HEAD, PREFIX_ROWS, SIZE_MAX,
                                    &covering) == TL_OK);
    CHECK(vector_create_prefix_int8(5, PREFIX_WIDE, PREFIX_ROWS, PREFIX_HEAD, 12, SIZE_MAX,
                                    &narrow) == TL_OK);
    float values[PREFIX_WIDE];
    for (size_t i = 0; i < PREFIX_ROWS; i++) {
        prefix_row(i, values);
        CHECK(vector_add(exact, i + 1, 5, values, PREFIX_WIDE) == TL_OK);
        CHECK(vector_add(covering, i + 1, 5, values, PREFIX_WIDE) == TL_OK);
        CHECK(vector_add(narrow, i + 1, 5, values, PREFIX_WIDE) == TL_OK);
    }
    tl_vector_workspace *a = sealed(exact), *b = sealed(covering), *c = sealed(narrow);
    tl_vector_result expected[PREFIX_ROWS], actual[PREFIX_ROWS];
    for (size_t target = 0; target < PREFIX_ROWS; target += 7) {
        prefix_row(target, values);
        size_t na = 0, nb = 0;
        for (size_t capacity = 1; capacity <= PREFIX_ROWS; capacity++) {
            CHECK(vector_query(exact, a, 5, values, PREFIX_WIDE, expected, capacity, &na) == TL_OK);
            CHECK(vector_query(covering, b, 5, values, PREFIX_WIDE, actual, capacity, &nb) ==
                  TL_OK);
            CHECK(na == capacity && nb == capacity);
            for (size_t i = 0; i < na; i++)
                CHECK(actual[i].id == expected[i].id && actual[i].cosine == expected[i].cosine);
        }
        /* A narrow shortlist still finds the row itself, with the same score. */
        CHECK(vector_query(narrow, c, 5, values, PREFIX_WIDE, actual, 3, &nb) == TL_OK && nb == 3);
        CHECK(actual[0].id == target + 1 && actual[0].id == expected[0].id &&
              actual[0].cosine == expected[0].cosine);
        for (size_t i = 0; i < nb; i++)
            for (size_t j = 0; j < PREFIX_ROWS; j++)
                CHECK(expected[j].id != actual[i].id || expected[j].cosine == actual[i].cosine);
    }
    vector_workspace_destroy(a);
    vector_workspace_destroy(b);
    vector_workspace_destroy(c);
    vector_destroy(exact);
    vector_destroy(covering);
    vector_destroy(narrow);
}

/* Unit vector e_i plus weight * e_j over PREFIX_NARROW components. */
static void axis_row(float *out, size_t i, float weight_i, size_t j, float weight_j) {
    for (size_t k = 0; k < PREFIX_NARROW; k++)
        out[k] = 0;
    out[i] += weight_i;
    out[j] += weight_j;
}

static tl_vector *axis_index(size_t shortlist, const float rows[][PREFIX_NARROW], size_t count) {
    tl_vector *index = NULL;
    CHECK(vector_create_prefix_int8(9, PREFIX_NARROW, count, PREFIX_HEAD, shortlist, SIZE_MAX,
                                    &index) == TL_OK);
    for (size_t i = 0; i < count; i++)
        CHECK(vector_add(index, i + 1, 9, rows[i], PREFIX_NARROW) == TL_OK);
    return index;
}

static size_t top_ids(tl_vector *index, tl_vector_workspace *workspace, const float *query,
                      size_t capacity, uint64_t *ids) {
    tl_vector_result results[4];
    size_t count = 0;
    CHECK(vector_query(index, workspace, 9, query, PREFIX_NARROW, results, capacity, &count) ==
          TL_OK);
    for (size_t i = 0; i < count; i++)
        ids[i] = results[i].id;
    return count;
}

/* The first pass keeps the best prefix cosines, so the full cosine can only
 * choose among them. Excluded rows take no slot; a zero prefix scans all. */
static void check_prefix_first_pass(void) {
    float rows[4][PREFIX_NARROW], query[PREFIX_NARROW];
    axis_row(rows[0], 0, 1, 16, -1);  /* prefix cosine 1, full cosine 0 */
    axis_row(rows[1], 0, 0.6F, 1, 0); /* prefix cosine 0.6, full cosine 0.8 */
    rows[1][1] = 0.8F;
    rows[1][16] = 1;
    axis_row(rows[2], 2, 1, 17, 1);
    axis_row(rows[3], 1, 1, 16, 1);
    axis_row(query, 0, 1, 16, 1);
    uint64_t ids[4];
    for (size_t shortlist = 1; shortlist <= 2; shortlist++) {
        tl_vector *index = axis_index(shortlist, (const float(*)[PREFIX_NARROW])rows, 4);
        tl_vector_workspace *workspace = sealed(index);
        CHECK(top_ids(index, workspace, query, 1, ids) == 1);
        CHECK(ids[0] == (shortlist == 1 ? 1 : 2));
        tl_vector_result results[3];
        size_t count = 9;
        CHECK(vector_query(index, workspace, 9, query, PREFIX_NARROW, results, shortlist + 1,
                           &count) == TL_LIMIT &&
              count == 0);
        const uint64_t excluded[] = {1};
        vector_workspace_exclude(workspace, excluded);
        CHECK(top_ids(index, workspace, query, 1, ids) == 1 && ids[0] == 2);
        vector_workspace_exclude(workspace, NULL);
        float tail_only[PREFIX_NARROW];
        axis_row(tail_only, 16, 1, 16, 0);
        CHECK(top_ids(index, workspace, tail_only, 1, ids) == 1 && ids[0] == 2);
        vector_workspace_destroy(workspace);
        vector_destroy(index);
    }
}

/* Equal prefix scores keep the earlier rows, whatever their full cosines:
 * only a shortlist of three reaches row 3, the best full match. */
static void check_prefix_ties(void) {
    float rows[4][PREFIX_NARROW], query[PREFIX_NARROW];
    for (size_t i = 0; i < 3; i++)
        axis_row(rows[i], 0, 1, 16 + i, 1);
    axis_row(rows[3], 1, 1, 16, 1);
    axis_row(query, 0, 1, 18, 1);
    uint64_t ids[4];
    for (size_t shortlist = 1; shortlist <= 3; shortlist++) {
        tl_vector *index = axis_index(shortlist, (const float(*)[PREFIX_NARROW])rows, 4);
        tl_vector_workspace *workspace = sealed(index);
        CHECK(top_ids(index, workspace, query, 1, ids) == 1 && ids[0] == (shortlist == 3 ? 3 : 1));
        vector_workspace_destroy(workspace);
        vector_destroy(index);
    }
}

static void check_prefix_limits(void) {
    tl_vector *index = NULL, *other = NULL;
    CHECK(vector_create_prefix_int8(1, 32, 4, 16, 2, SIZE_MAX, NULL) == TL_INVALID);
    CHECK(vector_create_prefix_int8(1, 32, 4, 0, 2, SIZE_MAX, &index) == TL_INVALID);
    CHECK(vector_create_prefix_int8(1, 32, 4, 32, 2, SIZE_MAX, &index) == TL_INVALID);
    CHECK(vector_create_prefix_int8(1, 40, 4, 24, 2, SIZE_MAX, &index) == TL_INVALID);
    CHECK(vector_create_prefix_int8(1, 32, 4, 16, 0, SIZE_MAX, &index) == TL_INVALID);
    CHECK(vector_create_prefix_int8(0, 32, 4, 16, 2, SIZE_MAX, &index) == TL_INVALID);
    CHECK(vector_create_prefix_int8(1, VECTOR_MAX_DIMENSIONS + 16, 4, 16, 2, SIZE_MAX, &index) ==
          TL_INVALID);
    CHECK(vector_create_prefix_int8(1, 32, SIZE_MAX, 16, 2, SIZE_MAX, &index) == TL_LIMIT);
    CHECK(index == NULL);
    CHECK(vector_create_int8(1, 32, 4, SIZE_MAX, &other) == TL_OK);
    CHECK(vector_create_prefix_int8(1, 32, 4, 16, 2, SIZE_MAX, &index) == TL_OK);
    /* Splitting rows costs one prefix inverse norm per row. */
    size_t bytes = vector_bytes(index);
    CHECK(bytes == vector_bytes(other) + 4 * sizeof(float));
    vector_destroy(index);
    CHECK(vector_create_prefix_int8(1, 32, 4, 16, 2, bytes - 1, &index) == TL_LIMIT);
    CHECK(vector_create_prefix_int8(1, 32, 4, 16, 2, bytes, &index) == TL_OK);
    vector_destroy(index);
    vector_destroy(other);
    CHECK(vector_create_prefix_int8(1, 32, 0, 16, 2, SIZE_MAX, &index) == TL_OK);
    tl_vector_workspace *workspace = sealed(index);
    float query[32] = {1};
    tl_vector_result result;
    size_t count = 9;
    CHECK(vector_query(index, workspace, 1, query, 32, &result, 1, &count) == TL_OK && count == 0);
    vector_workspace_destroy(workspace);
    vector_destroy(index);
}

/* Prefix rows copy bit-identically between prefix indexes (the shortlist may
 * differ) but not into a different prefix length or the unsplit int8 format. */
static void check_prefix_row_copy(void) {
    tl_vector *source = NULL, *copy = NULL, *longer = NULL, *unsplit = NULL;
    float values[PREFIX_WIDE];
    CHECK(vector_create_prefix_int8(5, PREFIX_WIDE, 2, PREFIX_HEAD, 1, SIZE_MAX, &source) == TL_OK);
    for (size_t i = 0; i < 2; i++) {
        prefix_row(i, values);
        CHECK(vector_add(source, i + 1, 5, values, PREFIX_WIDE) == TL_OK);
    }
    tl_vector_workspace *a = sealed(source);
    CHECK(vector_create_prefix_int8(5, PREFIX_WIDE, 2, PREFIX_HEAD, 2, SIZE_MAX, &copy) == TL_OK);
    CHECK(vector_create_prefix_int8(5, PREFIX_WIDE, 2, 32, 2, SIZE_MAX, &longer) == TL_OK);
    CHECK(vector_create_int8(5, PREFIX_WIDE, 2, SIZE_MAX, &unsplit) == TL_OK);
    CHECK(vector_add_row(longer, source, 0) == TL_INVALID);
    CHECK(vector_add_row(unsplit, source, 0) == TL_INVALID);
    CHECK(vector_add_row(copy, source, 0) == TL_OK && vector_add_row(copy, source, 1) == TL_OK);
    tl_vector_workspace *b = sealed(copy);
    tl_vector_result from_source[1], from_copy[2];
    size_t na = 0, nb = 0;
    CHECK(vector_query(source, a, 5, values, PREFIX_WIDE, from_source, 1, &na) == TL_OK);
    CHECK(vector_query(copy, b, 5, values, PREFIX_WIDE, from_copy, 2, &nb) == TL_OK && nb == 2);
    CHECK(na == 1 && from_copy[0].id == from_source[0].id &&
          from_copy[0].cosine == from_source[0].cosine);
    vector_workspace_destroy(a);
    vector_workspace_destroy(b);
    vector_destroy(source);
    vector_destroy(copy);
    vector_destroy(longer);
    vector_destroy(unsplit);
}

/* The largest size with the shortest tail: one dominant component over 4095
 * small ones makes long integer sums, and every self match still wins with the
 * exhaustive int8 score. */
static void check_prefix_extremes(void) {
    enum { ROWS = 3 };
    float(*rows)[VECTOR_MAX_DIMENSIONS] = calloc(ROWS, sizeof(*rows));
    CHECK(rows != NULL);
    for (size_t i = 0; i < ROWS; i++)
        for (size_t j = 0; j < VECTOR_MAX_DIMENSIONS; j++)
            rows[i][j] = j == i ? 64.0F : i == 2 ? -1.0F : 1.0F;
    tl_vector *exact = NULL, *index = NULL;
    CHECK(vector_create_int8(3, VECTOR_MAX_DIMENSIONS, ROWS, SIZE_MAX, &exact) == TL_OK);
    CHECK(vector_create_prefix_int8(3, VECTOR_MAX_DIMENSIONS, ROWS,
                                    VECTOR_MAX_DIMENSIONS - VECTOR_PREFIX_BLOCK, 1, SIZE_MAX,
                                    &index) == TL_OK);
    for (size_t i = 0; i < ROWS; i++) {
        CHECK(vector_add(exact, i + 1, 3, rows[i], VECTOR_MAX_DIMENSIONS) == TL_OK);
        CHECK(vector_add(index, i + 1, 3, rows[i], VECTOR_MAX_DIMENSIONS) == TL_OK);
    }
    tl_vector_workspace *a = sealed(exact), *b = sealed(index);
    for (size_t i = 0; i < ROWS; i++) {
        tl_vector_result expected, actual;
        size_t na = 0, nb = 0;
        CHECK(vector_query(exact, a, 3, rows[i], VECTOR_MAX_DIMENSIONS, &expected, 1, &na) ==
              TL_OK);
        CHECK(vector_query(index, b, 3, rows[i], VECTOR_MAX_DIMENSIONS, &actual, 1, &nb) == TL_OK);
        CHECK(na == 1 && nb == 1 && actual.id == i + 1 && expected.id == i + 1);
        CHECK(actual.cosine == expected.cosine);
    }
    vector_workspace_destroy(a);
    vector_workspace_destroy(b);
    vector_destroy(exact);
    vector_destroy(index);
    free(rows);
}

void test_vector(void) {
    check_prefix_matches_int8();
    check_prefix_first_pass();
    check_prefix_ties();
    check_prefix_limits();
    check_prefix_row_copy();
    check_prefix_extremes();
    check_row_copy();
    check_binary();
    check_int8();
    check_reference();
    check_normalization();
    check_lifecycle();
    check_empty_and_limits();
    check_ties_and_readers();
}
