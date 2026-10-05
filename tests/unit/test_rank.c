/* Analytic RRF scores, exact priority, byte ties and coherent source identities. */
#include "test.h"
#include "torchlight/rank.h"
#include <math.h>

static void check_fusion(void) {
    tl_rank *ranker = NULL;
    CHECK(rank_create(8, RANK_DEFAULT_RRF_K, &ranker) == TL_OK);
    const tl_rank_candidate lexical[] = {{1, "/root/exact", RANK_EXACT_PATH},
                                         {2, "/root/name", RANK_EXACT_BASENAME},
                                         {3, "/root/related", RANK_REGULAR},
                                         {4, "/root/unembedded", RANK_REGULAR}};
    const tl_rank_candidate semantic[] = {{3, "/root/related", RANK_REGULAR},
                                          {5, "/root/semantic-only", RANK_REGULAR},
                                          {2, "/root/name", RANK_REGULAR},
                                          {1, "/root/exact", RANK_REGULAR}};
    tl_rank_result results[8], head[1];
    size_t count = 0;
    CHECK(rank_fuse(ranker, lexical, 4, semantic, 4, results, 8, &count) == TL_OK && count == 5);
    CHECK(results[0].id == 1 && results[1].id == 2 && results[2].id == 3);
    CHECK(results[3].id == 5 && results[4].id == 4);
    CHECK(fabs(results[2].score - (1.0 / 63.0 + 1.0 / 61.0)) < 1e-15);
    CHECK(results[2].lexical_rank == 3 && results[2].semantic_rank == 1);
    CHECK(results[3].lexical_rank == 0 && results[4].semantic_rank == 0);
    CHECK(rank_fuse(ranker, lexical, 4, semantic, 4, head, 1, &count) == TL_OK && count == 1);
    CHECK(head[0].id == results[0].id && head[0].score == results[0].score);
    rank_destroy(ranker);
}

static void check_weak_exact_hit(void) {
    tl_rank *ranker = NULL;
    CHECK(rank_create(7, 1, &ranker) == TL_OK);
    const tl_rank_candidate lexical[] = {{1, "/r/lexical", RANK_REGULAR},
                                         {2, "/r/shared", RANK_REGULAR},
                                         {3, "/r/basename", RANK_EXACT_BASENAME},
                                         {4, "/r/path", RANK_EXACT_PATH}};
    const tl_rank_candidate semantic[] = {{2, "/r/shared", RANK_REGULAR},
                                          {5, "/r/semantic", RANK_REGULAR},
                                          {1, "/r/lexical", RANK_REGULAR}};
    tl_rank_result results[7];
    size_t count = 0;
    CHECK(rank_fuse(ranker, lexical, 4, semantic, 3, results, 7, &count) == TL_OK && count == 5);
    CHECK(results[0].id == 4 && results[1].id == 3 && results[2].id == 2);
    CHECK(results[0].score < results[2].score);
    /* Fallback must preserve the supplied lexical list verbatim. */
    CHECK(rank_fuse(ranker, lexical, 4, NULL, 0, results, 7, &count) == TL_OK && count == 4);
    for (size_t i = 0; i < count; i++)
        CHECK(results[i].id == lexical[i].id && results[i].semantic_rank == 0);
    rank_destroy(ranker);
}

static void check_ties_and_bytes(void) {
    tl_rank *ranker = NULL;
    CHECK(rank_create(4, RANK_DEFAULT_RRF_K, &ranker) == TL_OK);
    const tl_rank_candidate lexical[] = {{9, "/r/bad\xff", RANK_REGULAR},
                                         {8, "/r/earlier-bytes", RANK_REGULAR}};
    const tl_rank_candidate semantic[] = {{8, "/r/earlier-bytes", RANK_REGULAR},
                                          {9, "/r/bad\xff", RANK_REGULAR}};
    tl_rank_result results[4];
    size_t count = 0;
    CHECK(rank_fuse(ranker, lexical, 2, semantic, 2, results, 4, &count) == TL_OK && count == 2);
    CHECK(results[0].score == results[1].score && results[0].id == 9);
    const tl_rank_candidate duplicate_paths[] = {{(UINT64_C(1) << 62), "/r/same", RANK_REGULAR},
                                                 {UINT64_MAX, "/r/same", RANK_REGULAR}};
    CHECK(rank_fuse(ranker, NULL, 0, duplicate_paths, 2, results, 4, &count) == TL_OK);
    CHECK(results[0].id == (UINT64_C(1) << 62) && results[1].id == UINT64_MAX);
    rank_destroy(ranker);
}

static void check_errors(void) {
    tl_rank *ranker = NULL;
    CHECK(rank_create(0, 60, &ranker) == TL_INVALID && ranker == NULL);
    CHECK(rank_create(RANK_MAX_CANDIDATES + 1, 60, &ranker) == TL_INVALID);
    CHECK(rank_create(4, 0, &ranker) == TL_INVALID);
    CHECK(rank_create(4, UINT32_MAX, &ranker) == TL_OK);
    tl_rank_candidate lexical[] = {{1, "/r/one", RANK_REGULAR}, {1, "/r/one", RANK_REGULAR}};
    tl_rank_candidate semantic[] = {{1, "/r/replacement", RANK_REGULAR}};
    tl_rank_result results[4];
    size_t count = 99;
    CHECK(rank_fuse(ranker, lexical, 2, NULL, 0, results, 4, &count) == TL_INVALID && count == 0);
    CHECK(rank_fuse(ranker, NULL, 0, lexical, 2, results, 4, &count) == TL_INVALID && count == 0);
    CHECK(rank_fuse(ranker, lexical, 1, semantic, 1, results, 4, &count) == TL_STATE && count == 0);
    CHECK(rank_fuse(ranker, NULL, 1, NULL, 0, results, 4, &count) == TL_INVALID);
    CHECK(rank_fuse(ranker, lexical, SIZE_MAX, semantic, 1, results, 4, &count) == TL_LIMIT);
    CHECK(rank_fuse(ranker, lexical, 1, semantic, 4, results, 4, &count) == TL_LIMIT);
    CHECK(rank_fuse(ranker, lexical, 1, NULL, 0, results, 0, &count) == TL_INVALID);
    lexical[0].id = 0;
    CHECK(rank_fuse(ranker, lexical, 1, NULL, 0, results, 4, &count) == TL_INVALID);
    lexical[0].id = 1;
    lexical[0].priority = (tl_rank_priority)99;
    CHECK(rank_fuse(ranker, lexical, 1, NULL, 0, results, 4, &count) == TL_INVALID);
    lexical[0].priority = RANK_REGULAR;
    CHECK(rank_fuse(ranker, lexical, 1, NULL, 0, results, 4, &count) == TL_OK && count == 1);
    CHECK(rank_fuse(ranker, NULL, 0, NULL, 0, results, 4, &count) == TL_OK && count == 0);
    rank_destroy(ranker);
    rank_destroy(NULL);
}

void test_rank(void) {
    check_fusion();
    check_weak_exact_hit();
    check_ties_and_bytes();
    check_errors();
}
