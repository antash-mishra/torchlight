/* Owned float/int8 cosine and experimental sign shortlist; no query-time I/O. */
#include "torchlight/vector.h"
#include "torchlight/sort.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#define VECTOR_X86_POPCOUNT 1
#else
#define VECTOR_X86_POPCOUNT 0
#endif

struct tl_vector {
    uint64_t emb_gen, *ids;
    size_t dimensions, capacity, count, bytes;
    float *values;
    int8_t *quantized;
    float *inverse_norms;
    bool compact;
    uint64_t *binary;
    size_t shortlist;
    bool hardware_popcount;
    bool finished;
};
struct tl_vector_workspace {
    const tl_vector *index;
    float *query;
    const uint64_t *excluded; /* optional row bitmap, see vector_workspace_exclude */
    uint64_t signs[VECTOR_MAX_DIMENSIONS / 64];
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
    free(index->quantized);
    free(index->inverse_norms);
    free(index->binary);
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
    float normalized[VECTOR_MAX_DIMENSIONS];
    float *target = index->compact ? normalized : index->values + index->count * dimensions;
    tl_status status = vector_normalize(values, dimensions, target);
    if (status == TL_OK && index->compact) {
        uint32_t squared = 0;
        for (size_t i = 0; i < dimensions; i++) {
            int8_t value = (int8_t)roundf(normalized[i] * 127.0F);
            index->quantized[index->count * dimensions + i] = value;
            squared += (uint32_t)((int)value * (int)value);
        }
        if (squared == 0)
            return TL_STATE;
        index->inverse_norms[index->count] = 1.0F / sqrtf((float)squared);
        if (index->binary != NULL) {
            size_t words = (dimensions + 63) / 64;
            for (size_t i = 0; i < dimensions; i++)
                if (normalized[i] >= 0)
                    index->binary[index->count * words + i / 64] |= UINT64_C(1) << (i % 64);
        }
    }
    if (status == TL_OK)
        index->ids[index->count++] = id;
    return status;
}

tl_status vector_position(const tl_vector *index, uint64_t id, size_t *position) {
    if (index == NULL || position == NULL)
        return TL_INVALID;
    size_t low = 0, high = index->count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (index->ids[middle] < id)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == index->count || index->ids[low] != id)
        return TL_STATE;
    *position = low;
    return TL_OK;
}
tl_status vector_add_row(tl_vector *index, const tl_vector *source, size_t position) {
    if (index == NULL || source == NULL || position >= source->count ||
        index->dimensions != source->dimensions || index->compact != source->compact ||
        (index->binary == NULL) != (source->binary == NULL))
        return TL_INVALID;
    if (index->finished || index->emb_gen != source->emb_gen)
        return TL_STATE;
    uint64_t id = source->ids[position];
    if (index->count != 0 && id <= index->ids[index->count - 1])
        return TL_INVALID;
    if (index->count == index->capacity)
        return TL_LIMIT;
    size_t dimensions = index->dimensions, row = index->count;
    if (!index->compact) {
        memcpy(index->values + row * dimensions, source->values + position * dimensions,
               dimensions * sizeof(float));
    } else {
        memcpy(index->quantized + row * dimensions, source->quantized + position * dimensions,
               dimensions);
        index->inverse_norms[row] = source->inverse_norms[position];
        size_t words = (dimensions + 63) / 64;
        if (index->binary != NULL)
            memcpy(index->binary + row * words, source->binary + position * words,
                   words * sizeof(uint64_t));
    }
    index->ids[index->count++] = id;
    return TL_OK;
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
static double compact_cosine(const tl_vector *index, size_t row, const float *query) {
    const int8_t *values = index->quantized + row * index->dimensions;
    /* Independent accumulators permit SIMD without globally relaxing floating
     * point semantics. Quantization error is evaluated against the float oracle. */
    float sums[4] = {0};
    size_t i = 0;
    for (; i + 4 <= index->dimensions; i += 4)
        for (size_t j = 0; j < 4; j++)
            sums[j] += (float)values[i + j] * query[i + j];
    float score = sums[0] + sums[1] + sums[2] + sums[3];
    for (; i < index->dimensions; i++)
        score += (float)values[i] * query[i];
    double normalized = (double)score * index->inverse_norms[row];
    return normalized > 1 ? 1 : normalized < -1 ? -1 : normalized;
}

static unsigned popcount_portable(uint64_t value) {
    value -= (value >> 1) & UINT64_C(0x5555555555555555);
    value = (value & UINT64_C(0x3333333333333333)) + ((value >> 2) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return (unsigned)((value * UINT64_C(0x0101010101010101)) >> 56);
}
#if VECTOR_X86_POPCOUNT
__attribute__((target("popcnt"))) static unsigned
hamming_hardware(const uint64_t *left, const uint64_t *right, size_t words) {
    unsigned count = 0;
    for (size_t i = 0; i < words; i++)
        count += (unsigned)__builtin_popcountll(left[i] ^ right[i]);
    return count;
}
#endif
static unsigned hamming(const tl_vector *index, size_t row, const uint64_t *query) {
    size_t words = (index->dimensions + 63) / 64;
    const uint64_t *values = index->binary + row * words;
#if VECTOR_X86_POPCOUNT
    if (index->hardware_popcount)
        return hamming_hardware(values, query, words);
#endif
    unsigned distance = 0;
    for (size_t i = 0; i < words; i++)
        distance += popcount_portable(values[i] ^ query[i]);
    return distance;
}
static unsigned hamming_cutoff(const tl_vector *index, tl_vector_workspace *workspace,
                               size_t *tie_capacity) {
    size_t histogram[VECTOR_MAX_DIMENSIONS + 1] = {0};
    for (size_t i = 0; i < (index->dimensions + 63) / 64; i++)
        workspace->signs[i] = 0;
    for (size_t i = 0; i < index->dimensions; i++)
        if (workspace->query[i] >= 0)
            workspace->signs[i / 64] |= UINT64_C(1) << (i % 64);
    for (size_t row = 0; row < index->count; row++)
        histogram[hamming(index, row, workspace->signs)]++;
    size_t remaining = index->shortlist;
    for (unsigned distance = 0; distance <= index->dimensions; distance++) {
        if (histogram[distance] >= remaining) {
            *tie_capacity = remaining;
            return distance;
        }
        remaining -= histogram[distance];
    }
    *tie_capacity = remaining;
    return (unsigned)index->dimensions;
}

void vector_workspace_exclude(tl_vector_workspace *workspace, const uint64_t *excluded) {
    if (workspace != NULL)
        workspace->excluded = excluded;
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
    if (index->shortlist != 0 && capacity > index->shortlist)
        return TL_LIMIT;
    tl_status status = vector_normalize(query, dimensions, workspace->query);
    if (status != TL_OK)
        return status;
    size_t count = 0, tie_capacity = 0;
    bool prune = index->binary != NULL && index->count > index->shortlist;
    unsigned cutoff = prune ? hamming_cutoff(index, workspace, &tie_capacity) : 0;
    for (size_t row = 0; row < index->count; row++) {
        if (workspace->excluded != NULL && ((workspace->excluded[row / 64] >> (row % 64)) & 1U))
            continue;
        if (prune) {
            unsigned distance = hamming(index, row, workspace->signs);
            if (distance > cutoff || (distance == cutoff && tie_capacity == 0))
                continue;
            if (distance == cutoff)
                tie_capacity--;
        }
        double score = index->compact
                           ? compact_cosine(index, row, workspace->query)
                           : cosine(index->values + row * dimensions, workspace->query, dimensions);
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

tl_status vector_create_int8(uint64_t emb_gen, size_t dimensions, size_t capacity,
                             size_t budget_bytes, tl_vector **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (emb_gen == 0 || dimensions == 0 || dimensions > VECTOR_MAX_DIMENSIONS)
        return TL_INVALID;
    size_t components = 0, metadata = 0;
    if (tl_size_multiply(capacity, dimensions, &components) != TL_OK ||
        tl_size_multiply(capacity, sizeof(uint64_t) + sizeof(float), &metadata) != TL_OK ||
        metadata > SIZE_MAX - sizeof(tl_vector) ||
        components > SIZE_MAX - sizeof(tl_vector) - metadata)
        return TL_LIMIT;
    size_t bytes = sizeof(tl_vector) + components + metadata;
    if (bytes > budget_bytes)
        return TL_LIMIT;
    tl_vector *index = calloc(1, sizeof(*index));
    if (index == NULL)
        return TL_NOMEM;
    index->emb_gen = emb_gen;
    index->dimensions = dimensions;
    index->capacity = capacity;
    index->bytes = bytes;
    index->compact = true;
    if (capacity != 0) {
        index->ids = malloc(capacity * sizeof(*index->ids));
        index->inverse_norms = malloc(capacity * sizeof(*index->inverse_norms));
        index->quantized = malloc(components);
        if (index->ids == NULL || index->inverse_norms == NULL || index->quantized == NULL) {
            vector_destroy(index);
            return TL_NOMEM;
        }
    }
    *out = index;
    return TL_OK;
}

tl_status vector_create_binary_int8(uint64_t emb_gen, size_t dimensions, size_t capacity,
                                    size_t shortlist, size_t budget_bytes, tl_vector **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (shortlist == 0)
        return TL_INVALID;
    tl_vector *index = NULL;
    tl_status status = vector_create_int8(emb_gen, dimensions, capacity, budget_bytes, &index);
    if (status != TL_OK)
        return status;
    size_t words = (dimensions + 63) / 64, binary_bytes = 0;
    status = tl_size_multiply(capacity, words * sizeof(uint64_t), &binary_bytes);
    if (status == TL_OK && binary_bytes > budget_bytes - index->bytes)
        status = TL_LIMIT;
    if (status == TL_OK && capacity != 0) {
        index->binary = calloc(1, binary_bytes);
        if (index->binary == NULL)
            status = TL_NOMEM;
    }
    if (status != TL_OK) {
        vector_destroy(index);
        return status;
    }
    index->bytes += binary_bytes;
    index->shortlist = shortlist;
#if VECTOR_X86_POPCOUNT
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    index->hardware_popcount =
        __get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0 && (ecx & bit_POPCNT) != 0;
#endif
    *out = index;
    return TL_OK;
}
