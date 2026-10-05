/* RRF over caller-ranked lists; exact matches and lexical fallback stay stable. */
#include "torchlight/rank.h"
#include "torchlight/sort.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

struct tl_rank {
    tl_rank_result *candidates;
    size_t capacity;
    uint32_t rrf_k;
};

tl_status rank_create(size_t candidate_capacity, uint32_t rrf_k, tl_rank **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (candidate_capacity == 0 || candidate_capacity > RANK_MAX_CANDIDATES || rrf_k == 0)
        return TL_INVALID;
    tl_rank *ranker = calloc(1, sizeof(*ranker));
    if (ranker == NULL)
        return TL_NOMEM;
    ranker->candidates = malloc(candidate_capacity * sizeof(*ranker->candidates));
    if (ranker->candidates == NULL) {
        rank_destroy(ranker);
        return TL_NOMEM;
    }
    ranker->capacity = candidate_capacity;
    ranker->rrf_k = rrf_k;
    *out = ranker;
    return TL_OK;
}

void rank_destroy(tl_rank *ranker) {
    if (ranker == NULL)
        return;
    free(ranker->candidates);
    free(ranker);
}

static tl_status load_candidates(tl_rank *ranker, const tl_rank_candidate *input, size_t count,
                                 size_t offset, bool lexical) {
    for (size_t i = 0; i < count; i++) {
        const tl_rank_candidate *candidate = &input[i];
        if (candidate->id == 0 || candidate->path == NULL || candidate->path[0] == 0 ||
            (candidate->priority != RANK_REGULAR && candidate->priority != RANK_EXACT_BASENAME &&
             candidate->priority != RANK_EXACT_PATH))
            return TL_INVALID;
        ranker->candidates[offset + i] =
            (tl_rank_result){candidate->id,       candidate->path,
                             candidate->priority, 1.0 / ((double)ranker->rrf_k + (double)(i + 1)),
                             lexical ? i + 1 : 0, lexical ? 0 : i + 1};
    }
    return TL_OK;
}

static int compare_ids(const void *left, const void *right, const void *context) {
    const tl_rank_result *a = left, *b = right;
    (void)context;
    return a->id < b->id ? -1 : a->id > b->id ? 1 : 0;
}

static tl_status merge_candidates(tl_rank *ranker, size_t count, size_t *out_count) {
    tl_status status =
        sort_items(ranker->candidates, count, sizeof(*ranker->candidates), compare_ids, NULL);
    if (status != TL_OK)
        return status;
    size_t merged = 0;
    for (size_t i = 0; i < count; i++) {
        tl_rank_result candidate = ranker->candidates[i];
        if (merged == 0 || ranker->candidates[merged - 1].id != candidate.id) {
            ranker->candidates[merged++] = candidate;
            continue;
        }
        tl_rank_result *previous = &ranker->candidates[merged - 1];
        if ((previous->lexical_rank != 0 && candidate.lexical_rank != 0) ||
            (previous->semantic_rank != 0 && candidate.semantic_rank != 0))
            return TL_INVALID;
        if (strcmp(previous->path, candidate.path) != 0)
            return TL_STATE;
        previous->score += candidate.score;
        previous->lexical_rank += candidate.lexical_rank;
        previous->semantic_rank += candidate.semantic_rank;
        if (candidate.priority > previous->priority)
            previous->priority = candidate.priority;
    }
    *out_count = merged;
    return TL_OK;
}

static int compare_fused(const void *left, const void *right, const void *context) {
    const tl_rank_result *a = left, *b = right;
    bool lexical_only = *(const bool *)context;
    if (!lexical_only && a->priority != b->priority)
        return a->priority > b->priority ? -1 : 1;
    if (!lexical_only && a->score != b->score)
        return a->score > b->score ? -1 : 1;
    size_t a_rank = a->lexical_rank == 0 ? SIZE_MAX : a->lexical_rank;
    size_t b_rank = b->lexical_rank == 0 ? SIZE_MAX : b->lexical_rank;
    if (a_rank != b_rank)
        return a_rank < b_rank ? -1 : 1;
    int paths = strcmp(a->path, b->path);
    return paths != 0 ? paths : compare_ids(a, b, NULL);
}

tl_status rank_fuse(tl_rank *ranker, const tl_rank_candidate *lexical, size_t lexical_count,
                    const tl_rank_candidate *semantic, size_t semantic_count,
                    tl_rank_result *results, size_t capacity, size_t *out_count) {
    if (out_count != NULL)
        *out_count = 0;
    if (ranker == NULL || (lexical == NULL && lexical_count != 0) ||
        (semantic == NULL && semantic_count != 0) || results == NULL || out_count == NULL ||
        capacity == 0 || capacity > ranker->capacity)
        return TL_INVALID;
    if (lexical_count > ranker->capacity || semantic_count > ranker->capacity - lexical_count)
        return TL_LIMIT;
    tl_status status = load_candidates(ranker, lexical, lexical_count, 0, true);
    if (status == TL_OK)
        status = load_candidates(ranker, semantic, semantic_count, lexical_count, false);
    size_t count = 0;
    if (status == TL_OK)
        status = merge_candidates(ranker, lexical_count + semantic_count, &count);
    bool lexical_only = semantic_count == 0;
    if (status == TL_OK)
        status = sort_items(ranker->candidates, count, sizeof(*ranker->candidates), compare_fused,
                            &lexical_only);
    if (status != TL_OK)
        return status;
    *out_count = count < capacity ? count : capacity;
    memcpy(results, ranker->candidates, *out_count * sizeof(*results));
    return TL_OK;
}
