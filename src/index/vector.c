/* Exhaustive cosine reference over owned normalized floats; no query-time I/O. */
#include "torchlight/vector.h"
#include "torchlight/sort.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

struct tl_vector {
    uint64_t emb_gen, *ids;
    size_t dimensions, capacity, count, bytes;
    float *values;
    bool finished;
};
struct tl_vector_workspace {
    const tl_vector *index;
    float *query;
};

tl_status vector_normalize(const float *values, size_t dimensions, float *out) {
    if (values == NULL || out == NULL || dimensions == 0 || dimensions > VECTOR_MAX_DIMENSIONS)
        return TL_INVALID;
    double squared_norm = 0;
    for (size_t i = 0; i < dimensions; i++) {
        if (!isfinite(values[i]))
            return TL_INVALID;
        double value = values[i];
        squared_norm += value * value;
    }
    if (squared_norm == 0)
        return TL_INVALID;
    double norm = sqrt(squared_norm);
    for (size_t i = 0; i < dimensions; i++)
        out[i] = (float)((double)values[i] / norm);
    return TL_OK;
}

static tl_status storage_bytes(size_t capacity, size_t dimensions, size_t *floats, size_t *ids,
                               size_t *total) {
    size_t elements = 0;
    tl_status status = tl_size_multiply(capacity, dimensions, &elements);
    if (status == TL_OK)
        status = tl_size_multiply(elements, sizeof(float), floats);
    if (status == TL_OK)
        status = tl_size_multiply(capacity, sizeof(uint64_t), ids);
    if (status != TL_OK)
        return status;
    if (*ids > SIZE_MAX - sizeof(tl_vector) || *floats > SIZE_MAX - sizeof(tl_vector) - *ids)
        return TL_LIMIT;
    *total = sizeof(tl_vector) + *ids + *floats;
    return TL_OK;
}

tl_status vector_create(uint64_t emb_gen, size_t dimensions, size_t capacity, size_t budget_bytes,
                        tl_vector **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (emb_gen == 0 || dimensions == 0 || dimensions > VECTOR_MAX_DIMENSIONS)
        return TL_INVALID;
    size_t floats = 0, ids = 0, bytes = 0;
    tl_status status = storage_bytes(capacity, dimensions, &floats, &ids, &bytes);
    if (status != TL_OK)
        return status;
    if (bytes > budget_bytes)
        return TL_LIMIT;
    tl_vector *index = calloc(1, sizeof(*index));
    if (index == NULL)
        return TL_NOMEM;
    if (capacity != 0) {
        index->values = malloc(floats);
        index->ids = malloc(ids);
        if (index->values == NULL || index->ids == NULL) {
            vector_destroy(index);
            return TL_NOMEM;
        }
    }
    index->emb_gen = emb_gen;
    index->dimensions = dimensions;
    index->capacity = capacity;
    index->bytes = bytes;
    *out = index;
    return TL_OK;
}

void vector_destroy(tl_vector *index) {
    if (index == NULL)
        return;
    free(index->values);
    free(index->ids);
    free(index);
}

tl_status vector_add(tl_vector *index, uint64_t id, uint64_t emb_gen, const float *values,
                     size_t dimensions) {
    if (index == NULL || id == 0 || dimensions != index->dimensions || values == NULL)
        return TL_INVALID;
    if (index->finished || emb_gen != index->emb_gen)
        return TL_STATE;
    if (index->count != 0 && id <= index->ids[index->count - 1])
        return TL_INVALID;
    if (index->count == index->capacity)
        return TL_LIMIT;
    tl_status status =
        vector_normalize(values, dimensions, index->values + index->count * dimensions);
    if (status == TL_OK)
        index->ids[index->count++] = id;
    return status;
}

tl_status vector_finish(tl_vector *index) {
    if (index == NULL)
        return TL_INVALID;
    if (index->finished)
        return TL_STATE;
    index->finished = true;
    return TL_OK;
}

tl_status vector_workspace_create(const tl_vector *index, tl_vector_workspace **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (index == NULL)
        return TL_INVALID;
    if (!index->finished)
        return TL_STATE;
    size_t bytes = 0;
    tl_status status = tl_size_multiply(index->dimensions, sizeof(float), &bytes);
    if (status != TL_OK)
        return status;
    tl_vector_workspace *workspace = calloc(1, sizeof(*workspace));
    if (workspace == NULL)
        return TL_NOMEM;
    workspace->query = malloc(bytes);
    if (workspace->query == NULL) {
        vector_workspace_destroy(workspace);
        return TL_NOMEM;
    }
    workspace->index = index;
    *out = workspace;
    return TL_OK;
}

void vector_workspace_destroy(tl_vector_workspace *workspace) {
    if (workspace == NULL)
        return;
    free(workspace->query);
    free(workspace);
}

/* The heap root is the weakest retained hit. Including id in this total order
 * makes ties independent of output capacity and heap shape. */
static bool worse(tl_vector_result left, tl_vector_result right) {
    return left.cosine != right.cosine ? left.cosine < right.cosine : left.id > right.id;
}

static void sift_results(tl_vector_result *heap, size_t position, size_t count) {
    while (position < count / 2) {
        size_t child = position * 2 + 1;
        if (child + 1 < count && worse(heap[child + 1], heap[child]))
            child++;
        if (!worse(heap[child], heap[position]))
            return;
        tl_vector_result swap = heap[position];
        heap[position] = heap[child];
        heap[child] = swap;
        position = child;
    }
}

static void push_result(tl_vector_result *heap, size_t *count, size_t capacity,
                        tl_vector_result result) {
    if (*count == capacity) {
        if (worse(heap[0], result)) {
            heap[0] = result;
            sift_results(heap, 0, capacity);
        }
        return;
    }
    size_t position = (*count)++;
    while (position > 0 && worse(result, heap[(position - 1) / 2])) {
        heap[position] = heap[(position - 1) / 2];
        position = (position - 1) / 2;
    }
    heap[position] = result;
}

static int compare_results(const void *left, const void *right, const void *context) {
    const tl_vector_result *a = left, *b = right;
    (void)context;
    return worse(*a, *b) ? 1 : worse(*b, *a) ? -1 : 0;
}

static double cosine(const float *left, const float *right, size_t dimensions) {
    double score = 0;
    for (size_t i = 0; i < dimensions; i++)
        score += (double)left[i] * (double)right[i];
    /* Float normalization may round the norm slightly above one. */
    return score > 1 ? 1 : score < -1 ? -1 : score;
}

tl_status vector_query(const tl_vector *index, tl_vector_workspace *workspace, uint64_t emb_gen,
                       const float *query, size_t dimensions, tl_vector_result *results,
                       size_t capacity, size_t *out_count) {
    if (out_count != NULL)
        *out_count = 0;
    if (index == NULL || workspace == NULL || workspace->index != index || query == NULL ||
        dimensions != index->dimensions || results == NULL || out_count == NULL || capacity == 0 ||
        capacity > VECTOR_MAX_RESULTS)
        return TL_INVALID;
    if (!index->finished || emb_gen != index->emb_gen)
        return TL_STATE;
    tl_status status = vector_normalize(query, dimensions, workspace->query);
    if (status != TL_OK)
        return status;
    size_t count = 0;
    for (size_t row = 0; row < index->count; row++) {
        double score = cosine(index->values + row * dimensions, workspace->query, dimensions);
        push_result(results, &count, capacity, (tl_vector_result){index->ids[row], score});
    }
    status = sort_items(results, count, sizeof(*results), compare_results, NULL);
    if (status == TL_OK)
        *out_count = count;
    return status;
}

size_t vector_count(const tl_vector *index) {
    return index == NULL ? 0 : index->count;
}
uint64_t vector_emb_gen(const tl_vector *index) {
    return index == NULL ? 0 : index->emb_gen;
}
size_t vector_bytes(const tl_vector *index) {
    return index == NULL ? 0 : index->bytes;
}
