/* Bounded reciprocal rank fusion with explicit exact-match priority. */
#ifndef TORCHLIGHT_RANK_H
#define TORCHLIGHT_RANK_H
#include "torchlight/common.h"

#define RANK_DEFAULT_RRF_K 60
#define RANK_MAX_CANDIDATES 2000
typedef struct tl_rank tl_rank;
typedef enum { RANK_REGULAR, RANK_EXACT_BASENAME, RANK_EXACT_PATH } tl_rank_priority;
typedef struct {
    uint64_t id;
    const char *path;
    tl_rank_priority priority;
} tl_rank_candidate;
typedef struct {
    uint64_t id;
    const char *path;
    tl_rank_priority priority;
    double score;
    /* One-based source positions; zero means absent from that source. */
    size_t lexical_rank, semantic_rank;
} tl_rank_result;

/** Create owned scratch for 1..RANK_MAX_CANDIDATES combined input occurrences.
 * rrf_k must be positive; start with RANK_DEFAULT_RRF_K and tune on labels.
 * TL_INVALID/NOMEM; out NULL on failure. Use one ranker per concurrent query. */
tl_status rank_create(size_t candidate_capacity, uint32_t rrf_k, tl_rank **out);
/** Free owned scratch; borrowed input/output paths remain untouched.
 * NULL allowed, no errors. */
void rank_destroy(tl_rank *ranker);
/** Fuse best-first lexical/semantic lists with score=sum(1/(rrf_k+source_rank)).
 * Inputs may be NULL only for zero counts; ids must be nonzero and unique within
 * each list, paths nonempty raw byte strings, priorities valid. Across lists,
 * equal ids must have equal paths or TL_STATE prevents mixing catalog views.
 * Exact paths precede exact basenames, which precede ordinary fusion. Ties keep
 * lexical source order first, then raw path bytes and id. With no semantic
 * results, preserve lexical order exactly, including unembedded entries.
 * Copy up to capacity results (1..candidate_capacity); paths borrow input
 * lifetimes and no ownership transfers. Input/output arrays must not overlap.
 * TL_INVALID for arguments/duplicates, TL_LIMIT for scratch exhaustion;
 * out_count zero on errors and scratch stays reusable. No I/O/heap allocation.
 * Supply the same candidate lists across display limits for consistent top-k. */
tl_status rank_fuse(tl_rank *ranker, const tl_rank_candidate *lexical, size_t lexical_count,
                    const tl_rank_candidate *semantic, size_t semantic_count,
                    tl_rank_result *results, size_t capacity, size_t *out_count);
#endif
