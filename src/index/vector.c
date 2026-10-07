/* Owned float/int8 cosine, a two-pass prefix shortlist and an experimental
 * sign shortlist; no query-time I/O or allocation. */
#include "torchlight/vector.h"
#include "torchlight/sort.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#define VECTOR_X86 1
#else
#define VECTOR_X86 0
#endif

/* int8-l2-1: normalized components are rounded at this scale. */
static const float QUANTIZED_SCALE = 127.0F;
/* The first pass rounds the query prefix so its largest magnitude is this.
 * Rows hold |r| <= 127 and ||r|| <= 127 + sqrt(VECTOR_MAX_DIMENSIONS) / 2 (one
 * half per rounded component), and the rounded query has ||q|| <= 32767 *
 * sqrt(VECTOR_MAX_DIMENSIONS), so by Cauchy-Schwarz every partial dot product
 * stays below 32767 * 64 * 159 < 2^31: int32 sums cannot overflow. */
static const float PREFIX_QUERY_SCALE = 32767.0F;

struct tl_vector {
    uint64_t emb_gen, *ids;
    size_t dimensions, capacity, count, bytes;
    float *values;
    /* int8 rows. quantized holds each row's first prefix components; tail
     * holds the rest and is NULL unless a prefix shortlist splits rows, so an
     * unsplit row has prefix == dimensions. */
    int8_t *quantized, *tail;
    float *inverse_norms, *prefix_inverse_norms;
    size_t prefix;
    bool compact;
    uint64_t *binary;
    size_t shortlist;
    bool hardware_popcount, hardware_sse41, hardware_avx2;
    bool finished;
};
/* A first-pass prefix score; position orders ties. */
struct candidate {
    float score;
    size_t position;
};
/* The first pass scores rows in blocks so the SIMD kernel's call and the
 * candidate bookkeeping stay out of its inner loop. */
enum { PREFIX_BLOCK_ROWS = 64, RESCORE_PREFETCH_ROWS = 8 };
struct tl_vector_workspace {
    const tl_vector *index;
    float *query;
    const uint64_t *excluded; /* optional row bitmap, see vector_workspace_exclude */
    uint64_t signs[VECTOR_MAX_DIMENSIONS / 64];
    int16_t *prefix_query;        /* prefix shortlist only */
    struct candidate *candidates; /* prefix shortlist buffer, 2 * shortlist entries */
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
    index->prefix = dimensions;
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
    free(index->tail);
    free(index->inverse_norms);
    free(index->prefix_inverse_norms);
    free(index->binary);
    free(index->ids);
    free(index);
}

/* Row pointers into the split int8 storage; the tail is empty when unsplit. */
static int8_t *row_head(const tl_vector *index, size_t row) {
    return index->quantized + row * index->prefix;
}
static int8_t *row_tail(const tl_vector *index, size_t row) {
    return index->tail == NULL ? NULL : index->tail + row * (index->dimensions - index->prefix);
}
static int8_t *row_component(const tl_vector *index, size_t row, size_t component) {
    return component < index->prefix ? row_head(index, row) + component
                                     : row_tail(index, row) + (component - index->prefix);
}

/* Quantize a normalized row into the next int8 slot, with its inverse norms
 * and sign bits. TL_STATE leaves the slot unpublished (count unchanged). */
static tl_status quantize_row(tl_vector *index, const float *normalized) {
    size_t dimensions = index->dimensions, row = index->count;
    uint32_t squared = 0, prefix_squared = 0;
    for (size_t i = 0; i < dimensions; i++) {
        int8_t value = (int8_t)roundf(normalized[i] * QUANTIZED_SCALE);
        *row_component(index, row, i) = value;
        squared += (uint32_t)((int)value * (int)value);
        if (i + 1 == index->prefix)
            prefix_squared = squared;
    }
    if (squared == 0)
        return TL_STATE;
    index->inverse_norms[row] = 1.0F / sqrtf((float)squared);
    /* An all-zero prefix scores zero in the first pass rather than dividing by 0. */
    if (index->prefix_inverse_norms != NULL)
        index->prefix_inverse_norms[row] =
            prefix_squared == 0 ? 0.0F : 1.0F / sqrtf((float)prefix_squared);
    if (index->binary != NULL) {
        size_t words = (dimensions + 63) / 64;
        for (size_t i = 0; i < dimensions; i++)
            if (normalized[i] >= 0)
                index->binary[row * words + i / 64] |= UINT64_C(1) << (i % 64);
    }
    return TL_OK;
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
    if (status == TL_OK && index->compact)
        status = quantize_row(index, normalized);
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
        index->prefix != source->prefix || (index->binary == NULL) != (source->binary == NULL))
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
        memcpy(row_head(index, row), row_head(source, position), index->prefix);
        if (index->tail != NULL)
            memcpy(row_tail(index, row), row_tail(source, position), dimensions - index->prefix);
        index->inverse_norms[row] = source->inverse_norms[position];
        if (index->prefix_inverse_norms != NULL)
            index->prefix_inverse_norms[row] = source->prefix_inverse_norms[position];
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
    size_t bytes = 0, prefix_bytes = 0, candidate_bytes = 0;
    tl_status status = tl_size_multiply(index->dimensions, sizeof(float), &bytes);
    bool split = index->tail != NULL;
    if (status == TL_OK && split)
        status = tl_size_multiply(index->prefix, sizeof(int16_t), &prefix_bytes);
    if (status == TL_OK && split)
        status = tl_size_multiply(index->shortlist, 2 * sizeof(struct candidate), &candidate_bytes);
    if (status != TL_OK)
        return status;
    tl_vector_workspace *workspace = calloc(1, sizeof(*workspace));
    if (workspace == NULL)
        return TL_NOMEM;
    workspace->query = malloc(bytes);
    if (split) {
        workspace->prefix_query = malloc(prefix_bytes);
        workspace->candidates = malloc(candidate_bytes);
    }
    if (workspace->query == NULL ||
        (split && (workspace->prefix_query == NULL || workspace->candidates == NULL))) {
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
    free(workspace->prefix_query);
    free(workspace->candidates);
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
/* Independent accumulators permit SIMD without globally relaxing floating
 * point semantics. Quantization error is evaluated against the float oracle.
 * Lane j sums the products of components j, j + 4, ... in order. */
static void accumulate_portable(const int8_t *values, const float *query, size_t count,
                                float sums[4]) {
    for (size_t i = 0; i + 4 <= count; i += 4)
        for (size_t j = 0; j < 4; j++)
            sums[j] += (float)values[i + j] * query[i + j];
}
#if VECTOR_X86
/* The same lanes, conversions, products and sums as accumulate_portable, so
 * scores are bit-identical with or without SSE4.1. */
__attribute__((target("sse4.1"))) static void
accumulate_sse41(const int8_t *values, const float *query, size_t count, float sums[4]) {
    __m128 lanes = _mm_loadu_ps(sums);
    for (size_t i = 0; i + 4 <= count; i += 4) {
        int32_t packed = 0;
        memcpy(&packed, values + i, sizeof(packed));
        __m128 converted = _mm_cvtepi32_ps(_mm_cvtepi8_epi32(_mm_cvtsi32_si128(packed)));
        lanes = _mm_add_ps(lanes, _mm_mul_ps(converted, _mm_loadu_ps(query + i)));
    }
    _mm_storeu_ps(sums, lanes);
}
#endif
static void accumulate(const tl_vector *index, const int8_t *values, const float *query,
                       size_t count, float sums[4]) {
#if VECTOR_X86
    if (index->hardware_sse41) {
        accumulate_sse41(values, query, count, sums);
        return;
    }
#endif
    (void)index;
    accumulate_portable(values, query, count, sums);
}
/* A split row continues the head's accumulators over the tail. The prefix is a
 * multiple of 4, so every lane sees the same products in the same order as an
 * unsplit row: both layouts score bit-identically. */
static double compact_cosine(const tl_vector *index, size_t row, const float *query) {
    float sums[4] = {0};
    size_t rest = index->dimensions - index->prefix;
    accumulate(index, row_head(index, row), query, index->prefix, sums);
    if (rest != 0)
        accumulate(index, row_tail(index, row), query + index->prefix, rest, sums);
    float score = sums[0] + sums[1] + sums[2] + sums[3];
    for (size_t i = index->dimensions - index->dimensions % 4; i < index->dimensions; i++)
        score += (float)*row_component(index, row, i) * query[i];
    double normalized = (double)score * index->inverse_norms[row];
    return normalized > 1 ? 1 : normalized < -1 ? -1 : normalized;
}

static unsigned popcount_portable(uint64_t value) {
    value -= (value >> 1) & UINT64_C(0x5555555555555555);
    value = (value & UINT64_C(0x3333333333333333)) + ((value >> 2) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return (unsigned)((value * UINT64_C(0x0101010101010101)) >> 56);
}
#if VECTOR_X86
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
#if VECTOR_X86
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
static bool excluded_row(const tl_vector_workspace *workspace, size_t row) {
    return workspace->excluded != NULL && ((workspace->excluded[row / 64] >> (row % 64)) & 1U);
}
static double row_score(const tl_vector *index, const tl_vector_workspace *workspace, size_t row) {
    return index->compact ? compact_cosine(index, row, workspace->query)
                          : cosine(index->values + row * index->dimensions, workspace->query,
                                   index->dimensions);
}

/* ---- prefix shortlist ------------------------------------------------------ */

/* Exact integer dot products of rows [begin, end) with the int16 query
 * prefix; see PREFIX_QUERY_SCALE for the int32 bound. */
static void prefix_dots_portable(const tl_vector *index, const int16_t *query, size_t begin,
                                 size_t end, int32_t *out) {
    for (size_t row = begin; row < end; row++) {
        const int8_t *values = row_head(index, row);
        int32_t sum = 0;
        for (size_t i = 0; i < index->prefix; i++)
            sum += (int32_t)values[i] * (int32_t)query[i];
        out[row - begin] = sum;
    }
}
#if VECTOR_X86
/* Same integers as the portable loop, VECTOR_PREFIX_BLOCK components a step:
 * widen int8 to int16, then pairwise multiply-add into eight int32 lanes. */
__attribute__((target("avx2"))) static void prefix_dots_avx2(const tl_vector *index,
                                                             const int16_t *query, size_t begin,
                                                             size_t end, int32_t *out) {
    for (size_t row = begin; row < end; row++) {
        const int8_t *values = row_head(index, row);
        __m256i sums = _mm256_setzero_si256();
        for (size_t i = 0; i < index->prefix; i += VECTOR_PREFIX_BLOCK) {
            __m128i bytes = _mm_loadu_si128((const __m128i *)(const void *)(values + i));
            __m256i weights = _mm256_loadu_si256((const __m256i *)(const void *)(query + i));
            sums = _mm256_add_epi32(sums, _mm256_madd_epi16(_mm256_cvtepi8_epi16(bytes), weights));
        }
        __m128i half =
            _mm_add_epi32(_mm256_castsi256_si128(sums), _mm256_extracti128_si256(sums, 1));
        half = _mm_add_epi32(half, _mm_shuffle_epi32(half, _MM_SHUFFLE(1, 0, 3, 2)));
        half = _mm_add_epi32(half, _mm_shuffle_epi32(half, _MM_SHUFFLE(2, 3, 0, 1)));
        out[row - begin] = _mm_cvtsi128_si32(half);
    }
}
#endif
static void prefix_dots(const tl_vector *index, const int16_t *query, size_t begin, size_t end,
                        int32_t *out) {
#if VECTOR_X86
    if (index->hardware_avx2) {
        prefix_dots_avx2(index, query, begin, end, out);
        return;
    }
#endif
    prefix_dots_portable(index, query, begin, end, out);
}

/* Round the normalized query prefix to int16 at the largest scale that fits.
 * The scale is positive and shared by every row, so it cannot reorder rows.
 * Returns false for an all-zero prefix, which carries no ranking signal. */
static bool quantize_prefix(const tl_vector *index, tl_vector_workspace *workspace) {
    float largest = 0;
    for (size_t i = 0; i < index->prefix; i++)
        largest = fmaxf(largest, fabsf(workspace->query[i]));
    if (largest == 0)
        return false;
    float scale = PREFIX_QUERY_SCALE / largest;
    for (size_t i = 0; i < index->prefix; i++)
        workspace->prefix_query[i] = (int16_t)roundf(workspace->query[i] * scale);
    return true;
}

/* Total candidate order: higher score, then the earlier position. Positions
 * are unique, so the best k candidates form one well-defined set. */
static bool candidate_better(const struct candidate *left, const struct candidate *right) {
    return left->score != right->score ? left->score > right->score
                                       : left->position < right->position;
}
static void swap_candidates(struct candidate *left, struct candidate *right) {
    struct candidate swap = *left;
    *left = *right;
    *right = swap;
}
/* Partition [low, high] around its median-of-three; return the pivot's index. */
static size_t partition_candidates(struct candidate *items, size_t low, size_t high) {
    size_t middle = low + (high - low) / 2;
    if (candidate_better(&items[middle], &items[low]))
        swap_candidates(&items[middle], &items[low]);
    if (candidate_better(&items[high], &items[low]))
        swap_candidates(&items[high], &items[low]);
    if (candidate_better(&items[middle], &items[high]))
        swap_candidates(&items[middle], &items[high]);
    size_t store = low; /* items[high] is now the median pivot */
    for (size_t i = low; i < high; i++)
        if (candidate_better(&items[i], &items[high]))
            swap_candidates(&items[i], &items[store++]);
    swap_candidates(&items[store], &items[high]);
    return store;
}
static int compare_candidates(const void *left, const void *right, const void *context) {
    (void)context;
    return candidate_better(left, right) ? -1 : candidate_better(right, left) ? 1 : 0;
}
/* Move the best keep of count candidates (1 <= keep <= count) to the front,
 * with the keep-th best at keep - 1. Quickselect is linear on average; after
 * a bounded number of rounds the rest is heap-sorted, so the worst case stays
 * O(count log count) without allocation. */
static void select_candidates(struct candidate *items, size_t count, size_t keep) {
    size_t low = 0, high = count - 1, target = keep - 1, rounds = 0;
    while (low < high) {
        if (++rounds > 64) {
            tl_status sorted =
                sort_items(items + low, high - low + 1, sizeof(*items), compare_candidates, NULL);
            (void)sorted; /* valid arguments: cannot fail */
            return;
        }
        size_t pivot = partition_candidates(items, low, high);
        if (pivot == target)
            return;
        if (pivot < target)
            low = pivot + 1;
        else
            high = pivot - 1;
    }
}

/* The shortlist buffer holds kept candidates. Once trimmed, its first
 * shortlist entries are the best rows seen so far and the last of them is the
 * threshold a later row must beat; appends go after it. */
struct shortlist {
    size_t kept;
    bool trimmed;
};

/* Offer a block of first-pass scores to the shortlist buffer. Rows arrive in
 * increasing position, so a row tied with the threshold loses to it. When the
 * buffer holds twice the shortlist it is cut back to the best shortlist rows. */
static void offer_block(const tl_vector *index, tl_vector_workspace *workspace, size_t begin,
                        const int32_t *dots, size_t rows, struct shortlist *state) {
    struct candidate *buffer = workspace->candidates;
    size_t shortlist = index->shortlist;
    for (size_t i = 0; i < rows; i++) {
        size_t row = begin + i;
        if (excluded_row(workspace, row))
            continue;
        float score = (float)dots[i] * index->prefix_inverse_norms[row];
        if (state->trimmed && !(score > buffer[shortlist - 1].score))
            continue;
        buffer[state->kept++] = (struct candidate){score, row};
        if (state->kept == 2 * shortlist) {
            select_candidates(buffer, state->kept, shortlist);
            state->kept = shortlist;
            state->trimmed = true;
        }
    }
}

/* First pass over every searchable row's prefix, then full int8 cosines for
 * the best shortlist rows, prefetched a few rows ahead because they are
 * scattered. The result heap fixes the final order, so it does not depend on
 * the buffer's layout. */
static void prefix_search(const tl_vector *index, tl_vector_workspace *workspace,
                          tl_vector_result *results, size_t capacity, size_t *count) {
    struct shortlist state = {0};
    int32_t dots[PREFIX_BLOCK_ROWS];
    for (size_t begin = 0; begin < index->count; begin += PREFIX_BLOCK_ROWS) {
        size_t rows =
            index->count - begin < PREFIX_BLOCK_ROWS ? index->count - begin : PREFIX_BLOCK_ROWS;
        prefix_dots(index, workspace->prefix_query, begin, begin + rows, dots);
        offer_block(index, workspace, begin, dots, rows, &state);
    }
    size_t kept = state.kept;
    if (kept > index->shortlist) {
        select_candidates(workspace->candidates, kept, index->shortlist);
        kept = index->shortlist;
    }
    for (size_t i = 0; i < kept; i++) {
        if (i + RESCORE_PREFETCH_ROWS < kept) {
            size_t ahead = workspace->candidates[i + RESCORE_PREFETCH_ROWS].position;
            __builtin_prefetch(row_head(index, ahead));
            __builtin_prefetch(row_tail(index, ahead));
        }
        size_t row = workspace->candidates[i].position;
        push_result(
            results, count, capacity,
            (tl_vector_result){index->ids[row], compact_cosine(index, row, workspace->query)});
    }
}

/* Every searchable row, or the sign-bit shortlist when it prunes. */
static void scan_search(const tl_vector *index, tl_vector_workspace *workspace,
                        tl_vector_result *results, size_t capacity, size_t *count) {
    size_t tie_capacity = 0;
    bool prune = index->binary != NULL && index->count > index->shortlist;
    unsigned cutoff = prune ? hamming_cutoff(index, workspace, &tie_capacity) : 0;
    for (size_t row = 0; row < index->count; row++) {
        if (excluded_row(workspace, row))
            continue;
        if (prune) {
            unsigned distance = hamming(index, row, workspace->signs);
            if (distance > cutoff || (distance == cutoff && tie_capacity == 0))
                continue;
            if (distance == cutoff)
                tie_capacity--;
        }
        push_result(results, count, capacity,
                    (tl_vector_result){index->ids[row], row_score(index, workspace, row)});
    }
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
    size_t count = 0;
    /* Excluded rows never take shortlist slots, so counting them here only
     * runs the first pass where an exhaustive scan would also have fit. */
    if (index->tail != NULL && index->count > index->shortlist && quantize_prefix(index, workspace))
        prefix_search(index, workspace, results, capacity, &count);
    else
        scan_search(index, workspace, results, capacity, &count);
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

/* Bytes of an int8 index: struct, ids, components and one or two inverse
 * norms per row (split rows add prefix norms). TL_LIMIT on overflow. */
static tl_status compact_bytes(size_t capacity, size_t dimensions, bool split, size_t *out) {
    size_t components = 0, metadata = 0;
    size_t per_row = sizeof(uint64_t) + sizeof(float) * (split ? 2 : 1);
    if (tl_size_multiply(capacity, dimensions, &components) != TL_OK ||
        tl_size_multiply(capacity, per_row, &metadata) != TL_OK ||
        metadata > SIZE_MAX - sizeof(tl_vector) ||
        components > SIZE_MAX - sizeof(tl_vector) - metadata)
        return TL_LIMIT;
    *out = sizeof(tl_vector) + components + metadata;
    return TL_OK;
}

/* Shared int8 constructor: rows keep their first prefix components in
 * quantized and, when prefix < dimensions, the rest in tail. */
static tl_status create_compact(uint64_t emb_gen, size_t dimensions, size_t capacity, size_t prefix,
                                size_t budget_bytes, tl_vector **out) {
    *out = NULL;
    if (emb_gen == 0 || dimensions == 0 || dimensions > VECTOR_MAX_DIMENSIONS)
        return TL_INVALID;
    bool split = prefix < dimensions;
    size_t bytes = 0;
    tl_status status = compact_bytes(capacity, dimensions, split, &bytes);
    if (status != TL_OK)
        return status;
    if (bytes > budget_bytes)
        return TL_LIMIT;
    tl_vector *index = calloc(1, sizeof(*index));
    if (index == NULL)
        return TL_NOMEM;
    *index = (tl_vector){.emb_gen = emb_gen,
                         .dimensions = dimensions,
                         .prefix = prefix,
                         .capacity = capacity,
                         .bytes = bytes,
                         .compact = true};
#if VECTOR_X86
    index->hardware_sse41 = __builtin_cpu_supports("sse4.1") != 0;
    index->hardware_avx2 = __builtin_cpu_supports("avx2") != 0;
#endif
    if (capacity != 0) {
        /* compact_bytes proved capacity * dimensions (and so each part) fits. */
        index->ids = malloc(capacity * sizeof(*index->ids));
        index->inverse_norms = malloc(capacity * sizeof(*index->inverse_norms));
        index->quantized = malloc(capacity * prefix);
        if (split) {
            index->tail = malloc(capacity * (dimensions - prefix));
            index->prefix_inverse_norms = malloc(capacity * sizeof(float));
        }
        if (index->ids == NULL || index->inverse_norms == NULL || index->quantized == NULL ||
            (split && (index->tail == NULL || index->prefix_inverse_norms == NULL))) {
            vector_destroy(index);
            return TL_NOMEM;
        }
    }
    *out = index;
    return TL_OK;
}

tl_status vector_create_int8(uint64_t emb_gen, size_t dimensions, size_t capacity,
                             size_t budget_bytes, tl_vector **out) {
    if (out == NULL)
        return TL_INVALID;
    return create_compact(emb_gen, dimensions, capacity, dimensions, budget_bytes, out);
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
#if VECTOR_X86
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    index->hardware_popcount =
        __get_cpuid(1, &eax, &ebx, &ecx, &edx) != 0 && (ecx & bit_POPCNT) != 0;
#endif
    *out = index;
    return TL_OK;
}

tl_status vector_create_prefix_int8(uint64_t emb_gen, size_t dimensions, size_t capacity,
                                    size_t prefix_dimensions, size_t shortlist, size_t budget_bytes,
                                    tl_vector **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (shortlist == 0 || prefix_dimensions == 0 || prefix_dimensions >= dimensions ||
        prefix_dimensions % VECTOR_PREFIX_BLOCK != 0)
        return TL_INVALID;
    tl_vector *index = NULL;
    tl_status status =
        create_compact(emb_gen, dimensions, capacity, prefix_dimensions, budget_bytes, &index);
    if (status != TL_OK)
        return status;
    index->shortlist = shortlist;
    *out = index;
    return TL_OK;
}
