/* Persisted-state map: lookups, seen marks, replacement, removal, compaction
 * and the incomplete state after misuse. */
#include "test.h"
#include "torchlight/ledger.h"
#include <stdio.h>
static size_t unseen_count(const tl_ledger *ledger, uint64_t *first) {
    tl_vec *ids = NULL;
    CHECK(vec_create(sizeof(uint64_t), &ids) == TL_OK);
    CHECK(ledger_unseen(ledger, ids) == TL_OK);
    size_t count = vec_count(ids);
    if (count != 0 && first != NULL)
        *first = ((const uint64_t *)vec_const_data(ids))[0];
    vec_destroy(ids);
    return count;
}
static void lookups_and_seen_marks(void) {
    tl_ledger *ledger = NULL;
    CHECK(ledger_create(&ledger) == TL_OK && !ledger_complete(ledger));
    CHECK(ledger_put(ledger, 1, "/home/a", 11) == TL_OK &&
          ledger_put(ledger, 2, "/home/b", 22) == TL_OK &&
          ledger_put(ledger, 5, "/home/c", 55) == TL_OK);
    ledger_seal(ledger);
    CHECK(ledger_complete(ledger) && ledger_count(ledger) == 3);
    uint64_t id = 0, signature = 0, first = 0;
    CHECK(ledger_find(ledger, "/home/b", &id, &signature) && id == 2 && signature == 22);
    CHECK(!ledger_find(ledger, "/home/missing", &id, &signature));
    CHECK(!ledger_find(ledger, "/home", &id, &signature)); /* a prefix is another path */
    CHECK(ledger_begin(ledger) == TL_OK && unseen_count(ledger, &first) == 3 && first == 1);
    CHECK(ledger_find(ledger, "/home/a", &id, &signature) && id == 1);
    CHECK(unseen_count(ledger, &first) == 2 && first == 2);
    /* A rename after commit: the same id now lives at another path. */
    CHECK(ledger_put(ledger, 2, "/home/renamed", 23) == TL_OK);
    CHECK(!ledger_find(ledger, "/home/b", &id, &signature));
    CHECK(ledger_find(ledger, "/home/renamed", &id, &signature) && id == 2 && signature == 23);
    ledger_remove(ledger, 5);
    ledger_remove(ledger, 5); /* absent: no-op */
    CHECK(!ledger_contains(ledger, 5) && ledger_count(ledger) == 2);
    CHECK(!ledger_find(ledger, "/home/c", &id, &signature));
    /* A removed row reloaded after a commit comes back in place. */
    CHECK(ledger_put(ledger, 5, "/home/c", 56) == TL_OK && ledger_contains(ledger, 5));
    CHECK(ledger_find(ledger, "/home/c", &id, &signature) && signature == 56);
    CHECK(ledger_put(ledger, 9, "/home/new", 99) == TL_OK && ledger_count(ledger) == 4);
    CHECK(ledger_complete(ledger));
    ledger_reset(ledger);
    CHECK(!ledger_complete(ledger) && ledger_count(ledger) == 0);
    CHECK(!ledger_find(ledger, "/home/a", &id, &signature));
    ledger_destroy(ledger);
}
/* Ids only grow in the catalog; anything else, or an invalid row, leaves the
 * ledger incomplete until a reset and a clean reload. */
static void misuse_makes_it_incomplete(void) {
    tl_ledger *ledger = NULL;
    CHECK(ledger_create(&ledger) == TL_OK);
    CHECK(ledger_put(ledger, 10, "/x", 1) == TL_OK && ledger_put(ledger, 4, "/y", 2) == TL_STATE);
    ledger_seal(ledger);
    CHECK(!ledger_complete(ledger));
    ledger_reset(ledger);
    CHECK(ledger_put(ledger, 1, "/x", 1) == TL_OK);
    ledger_seal(ledger);
    CHECK(ledger_complete(ledger));
    CHECK(ledger_put(ledger, 0, "/z", 1) == TL_INVALID && !ledger_complete(ledger));
    ledger_seal(ledger); /* a failure since the reset keeps it incomplete */
    CHECK(!ledger_complete(ledger));
    ledger_reset(ledger);
    ledger_seal(ledger);
    ledger_invalidate(ledger);
    CHECK(!ledger_complete(ledger));
    CHECK(ledger_put(NULL, 1, "/x", 1) == TL_INVALID && ledger_begin(NULL) == TL_INVALID);
    CHECK(!ledger_complete(NULL) && ledger_count(NULL) == 0 && !ledger_contains(NULL, 1));
    ledger_destroy(ledger);
}
#define LEDGER_TEST_ROWS 3000
#define LEDGER_TEST_REMOVED 2000
/* Removing most rows compacts storage; survivors keep their ids, paths and
 * seen marks work on the new layout. */
static void compaction_keeps_survivors(void) {
    tl_ledger *ledger = NULL;
    CHECK(ledger_create(&ledger) == TL_OK);
    char path[64];
    for (uint64_t id = 1; id <= LEDGER_TEST_ROWS; id++) {
        CHECK(snprintf(path, sizeof(path), "/tree/file-%llu", (unsigned long long)id) > 0);
        CHECK(ledger_put(ledger, id, path, id * 7) == TL_OK);
    }
    ledger_seal(ledger);
    for (uint64_t id = 1; id <= LEDGER_TEST_REMOVED; id++)
        ledger_remove(ledger, id);
    CHECK(ledger_complete(ledger) &&
          ledger_count(ledger) == LEDGER_TEST_ROWS - LEDGER_TEST_REMOVED);
    CHECK(ledger_begin(ledger) == TL_OK);
    uint64_t id = 0, signature = 0, first = 0;
    for (uint64_t wanted = LEDGER_TEST_REMOVED + 1; wanted <= LEDGER_TEST_ROWS; wanted += 2) {
        CHECK(snprintf(path, sizeof(path), "/tree/file-%llu", (unsigned long long)wanted) > 0);
        CHECK(ledger_find(ledger, path, &id, &signature) && id == wanted &&
              signature == wanted * 7);
    }
    CHECK(!ledger_find(ledger, "/tree/file-1", &id, &signature));
    CHECK(unseen_count(ledger, &first) == (LEDGER_TEST_ROWS - LEDGER_TEST_REMOVED) / 2 &&
          first == LEDGER_TEST_REMOVED + 2);
    CHECK(ledger_put(ledger, LEDGER_TEST_ROWS + 1, "/tree/after", 1) == TL_OK);
    CHECK(ledger_find(ledger, "/tree/after", &id, &signature) && id == LEDGER_TEST_ROWS + 1);
    ledger_destroy(ledger);
}
void test_ledger(void) {
    lookups_and_seen_marks();
    misuse_makes_it_incomplete();
    compaction_keeps_survivors();
}
