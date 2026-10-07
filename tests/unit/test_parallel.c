/* Worker partitions cover each index once and remain reusable after errors. */
#include "test.h"
#include "torchlight/parallel.h"
struct counters {
    unsigned visits[131];
    size_t owner[131];
    bool fail;
};
static tl_status visit(void *context, size_t participant, size_t begin, size_t end) {
    struct counters *counters = context;
    CHECK(participant < PARALLEL_MAX_PARTICIPANTS);
    for (size_t i = begin; i < end; i++) {
        counters->visits[i]++;
        counters->owner[i] = participant;
    }
    return counters->fail && begin <= 65 && end > 65 ? TL_STATE : TL_OK;
}
void test_parallel(void) {
    tl_parallel *pool = NULL;
    CHECK(parallel_create(0, &pool) == TL_INVALID && pool == NULL);
    CHECK(parallel_create(PARALLEL_MAX_PARTICIPANTS + 1, &pool) == TL_INVALID);
    for (size_t participants = 1; participants <= PARALLEL_MAX_PARTICIPANTS; participants++) {
        CHECK(parallel_create(participants, &pool) == TL_OK);
        for (size_t count = 0; count <= 131; count++) {
            struct counters counters = {0};
            CHECK(parallel_run(pool, count, visit, &counters) == TL_OK);
            for (size_t i = 0; i < 131; i++)
                CHECK(counters.visits[i] == (i < count ? 1U : 0U));
            /* Participants own contiguous ranges in index order, the caller first. */
            for (size_t i = 1; i < count; i++)
                CHECK(counters.owner[i] >= counters.owner[i - 1] &&
                      counters.owner[i] < participants);
            CHECK(count == 0 || counters.owner[0] == 0);
        }
        struct counters counters = {.fail = true};
        CHECK(parallel_run(pool, 131, visit, &counters) == TL_STATE);
        counters.fail = false;
        CHECK(parallel_run(pool, 131, visit, &counters) == TL_OK);
        for (size_t i = 0; i < 131; i++)
            CHECK(counters.visits[i] == 2);
        CHECK(parallel_run(pool, 1, NULL, NULL) == TL_INVALID);
        parallel_destroy(pool);
    }
    parallel_destroy(NULL);
}
