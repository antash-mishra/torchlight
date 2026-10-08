/* Usage summary: boost values, decay, retention, query-to-open prefixes,
 * capacity eviction, clearing and merging. */
#include "test.h"
#include "torchlight/usage.h"
#include <string.h>

enum { DAY = 24 * 3600, RETENTION = 30 * DAY, START = 1000000 };

static tl_usage_target file(uint64_t id) {
    return (tl_usage_target){id, NULL};
}
static tl_usage_target app(const char *desktop_id) {
    return (tl_usage_target){0, desktop_id};
}
static size_t index_of(const tl_usage *usage, tl_usage_target target) {
    for (size_t i = 0; i < usage_count(usage); i++) {
        tl_usage_target found = usage_target(usage, i);
        if (target.file_id != 0
                ? found.file_id == target.file_id
                : found.desktop_id != NULL && strcmp(found.desktop_id, target.desktop_id) == 0)
            return i;
    }
    return SIZE_MAX;
}
static int boost(tl_usage *usage, const char *query, int64_t now, tl_usage_target target) {
    const int *boosts = NULL;
    CHECK(usage_boosts(usage, query, now, &boosts) == TL_OK);
    size_t index = index_of(usage, target);
    return index == SIZE_MAX ? 0 : boosts[index];
}
static void contract(void) {
    tl_usage *usage = NULL;
    CHECK(usage_create(0, &usage) == TL_INVALID && usage == NULL);
    CHECK(usage_create(RETENTION, NULL) == TL_INVALID);
    CHECK(usage_create(RETENTION, &usage) == TL_OK);
    CHECK(usage_record(NULL, file(1), NULL, START) == TL_INVALID);
    CHECK(usage_record(usage, file(0), NULL, START) == TL_INVALID);
    CHECK(usage_record(usage, (tl_usage_target){1, "x.desktop"}, NULL, START) == TL_INVALID);
    CHECK(usage_record(usage, app(""), NULL, START) == TL_INVALID);
    CHECK(usage_record(usage, file(1), NULL, -1) == TL_INVALID);
    char long_id[USAGE_DESKTOP_ID_BYTES + 1];
    memset(long_id, 'a', USAGE_DESKTOP_ID_BYTES);
    long_id[USAGE_DESKTOP_ID_BYTES] = 0;
    CHECK(usage_record(usage, app(long_id), NULL, START) == TL_LIMIT && usage_count(usage) == 0);
    const int *boosts = NULL;
    CHECK(usage_boosts(usage, NULL, START, &boosts) == TL_INVALID);
    CHECK(usage_merge(usage, usage) == TL_INVALID && usage_merge(NULL, usage) == TL_INVALID);
    CHECK(usage_target(usage, 0).file_id == 0 && usage_target(usage, 0).desktop_id == NULL);
    CHECK(usage_version(NULL) == 0 && usage_count(NULL) == 0);
    usage_clear(NULL);
    usage_destroy(usage);
    usage_destroy(NULL);
}
/* Saturating frecency, two-week half-life and retention eviction. */
static void frecency(void) {
    tl_usage *usage = NULL;
    CHECK(usage_create(RETENTION, &usage) == TL_OK);
    uint64_t version = usage_version(usage);
    CHECK(usage_record(usage, file(7), NULL, START) == TL_OK);
    CHECK(usage_version(usage) != version && usage_count(usage) == 1);
    CHECK(boost(usage, "", START, file(7)) == 133); /* 400 * 1 / (1 + 2) */
    version = usage_version(usage);
    CHECK(usage_record(usage, file(7), NULL, START) == TL_OK);
    CHECK(usage_version(usage) == version); /* known item: indices unchanged */
    CHECK(boost(usage, "", START, file(7)) == 200);
    /* Two weeks halve the weight: 400 * 1 / (1 + 2). */
    CHECK(boost(usage, "", START + 14 * DAY, file(7)) == 133);
    /* Out-of-order opens add their decayed weight. */
    CHECK(usage_record(usage, file(8), NULL, START + 14 * DAY) == TL_OK);
    CHECK(usage_record(usage, file(8), NULL, START) == TL_OK);
    CHECK(boost(usage, "", START + 14 * DAY, file(8)) == 171); /* weight 1.5 */
    CHECK(boost(usage, "", START + RETENTION, file(7)) > 0);
    version = usage_version(usage);
    /* Cached frecency refreshes at most once a minute, so expiry shows then. */
    CHECK(boost(usage, "", START + RETENTION + 61, file(7)) == 0);
    CHECK(usage_count(usage) == 1 && index_of(usage, file(7)) == SIZE_MAX);
    CHECK(usage_version(usage) != version);
    usage_destroy(usage);
}
/* Stored queries boost their targets for every typed prefix. */
static void query_history(void) {
    tl_usage *usage = NULL;
    CHECK(usage_create(RETENTION, &usage) == TL_OK);
    CHECK(usage_record(usage, app("google-chrome.desktop"), "  Chrome   Beta ", START) == TL_OK);
    CHECK(usage_record(usage, file(3), "CHAPTER", START) == TL_OK);
    tl_usage_target chrome = app("google-chrome.desktop");
    CHECK(boost(usage, "", START, chrome) == 133);
    CHECK(boost(usage, "c", START, chrome) == 733); /* 133 + 1200 * 1 / (1 + 1) */
    CHECK(boost(usage, "chrome b", START, chrome) == 733);
    CHECK(boost(usage, "CHROME  BETA", START, chrome) == 733);
    CHECK(boost(usage, "chrome beta 2", START, chrome) == 133);
    CHECK(boost(usage, "chx", START, chrome) == 133);
    CHECK(boost(usage, "ch", START, file(3)) == 733 && boost(usage, "chr", START, file(3)) == 133);
    /* A query opening two targets boosts both; repeats accumulate. */
    CHECK(usage_record(usage, file(3), "chrome", START) == TL_OK);
    CHECK(boost(usage, "chrome", START, file(3)) > boost(usage, "chapter", START, chrome));
    /* Typed queries longer than the stored length get no query boost. */
    char typed[USAGE_QUERY_SYMBOLS + 2];
    memset(typed, 'c', sizeof(typed) - 1);
    typed[sizeof(typed) - 1] = 0;
    CHECK(usage_record(usage, file(4), typed, START) == TL_OK);
    CHECK(boost(usage, "ccc", START, file(4)) == 733 && boost(usage, typed, START, file(4)) == 133);
    usage_destroy(usage);
}
/* Full tables evict their weakest entries; boosts stay correct. */
static void capacity(void) {
    tl_usage *usage = NULL;
    CHECK(usage_create(RETENTION, &usage) == TL_OK);
    for (uint64_t id = 1; id <= USAGE_MAX_ITEMS; id++)
        CHECK(usage_record(usage, file(id), NULL, START + (int64_t)id) == TL_OK);
    CHECK(usage_record(usage, file(1), NULL, START + USAGE_MAX_ITEMS) == TL_OK);
    uint64_t version = usage_version(usage);
    CHECK(usage_record(usage, file(USAGE_MAX_ITEMS + 1), "new", START + USAGE_MAX_ITEMS) == TL_OK);
    CHECK(usage_count(usage) == USAGE_MAX_ITEMS && usage_version(usage) != version);
    /* The oldest single open (id 2) went; the reopened id 1 stayed. */
    CHECK(index_of(usage, file(2)) == SIZE_MAX && index_of(usage, file(1)) != SIZE_MAX);
    CHECK(boost(usage, "ne", START + USAGE_MAX_ITEMS, file(USAGE_MAX_ITEMS + 1)) == 733);
    usage_clear(usage);
    CHECK(usage_count(usage) == 0);
    /* More pairs than fit: the weakest pairs go, the newest are found. */
    char query[32];
    for (int i = 0; i < USAGE_MAX_PAIRS + 10; i++) {
        snprintf(query, sizeof(query), "q%05d", i);
        CHECK(usage_record(usage, file(1), query, START + i) == TL_OK);
    }
    int64_t now = START + USAGE_MAX_PAIRS + 10;
    CHECK(boost(usage, "q00000", now, file(1)) < boost(usage, "q04105", now, file(1)));
    usage_destroy(usage);
}
/* Eviction compares weights as of one common time, even when timestamps
 * arrive out of order: one open four weeks (two half-lives) later is worth
 * four opens now, so it outweighs two opens now. Regression: entries newer
 * than the incoming open were compared undecayed, evicting the stronger one. */
static void eviction_order(void) {
    enum { FILLER_OPENS = 5, LATER = 28 * DAY };
    tl_usage *usage = NULL;
    CHECK(usage_create(RETENTION, &usage) == TL_OK);
    /* Items: file 1 opened once later, file 2 twice now, fillers five times. */
    for (uint64_t id = 3; id <= USAGE_MAX_ITEMS; id++)
        for (int k = 0; k < FILLER_OPENS; k++)
            CHECK(usage_record(usage, file(id), NULL, START) == TL_OK);
    CHECK(usage_record(usage, file(1), NULL, START + LATER) == TL_OK);
    CHECK(usage_record(usage, file(2), NULL, START) == TL_OK);
    CHECK(usage_record(usage, file(2), NULL, START) == TL_OK);
    CHECK(usage_count(usage) == USAGE_MAX_ITEMS);
    CHECK(usage_record(usage, file(USAGE_MAX_ITEMS + 1), NULL, START) == TL_OK);
    CHECK(index_of(usage, file(1)) != SIZE_MAX && index_of(usage, file(2)) == SIZE_MAX);
    /* Pairs: the same shape with stored queries of one file. */
    usage_clear(usage);
    char query[32];
    for (int i = 0; i < USAGE_MAX_PAIRS - 2; i++) {
        snprintf(query, sizeof(query), "f%05d", i);
        for (int k = 0; k < FILLER_OPENS; k++)
            CHECK(usage_record(usage, file(1), query, START) == TL_OK);
    }
    CHECK(usage_record(usage, file(1), "alpha", START + LATER) == TL_OK);
    CHECK(usage_record(usage, file(1), "beta", START) == TL_OK);
    CHECK(usage_record(usage, file(1), "beta", START) == TL_OK);
    CHECK(usage_record(usage, file(1), "gamma", START) == TL_OK);
    int64_t now = START + LATER;
    int unmatched = boost(usage, "zzz", now, file(1));
    CHECK(boost(usage, "alpha", now, file(1)) > unmatched);
    CHECK(boost(usage, "beta", now, file(1)) == unmatched);
    usage_destroy(usage);
}
/* Merging equals recording everything in one summary. */
static void merge(void) {
    tl_usage *all = NULL, *left = NULL, *right = NULL;
    CHECK(usage_create(RETENTION, &all) == TL_OK && usage_create(RETENTION, &left) == TL_OK &&
          usage_create(RETENTION, &right) == TL_OK);
    struct {
        tl_usage_target target;
        const char *query;
        int64_t at;
        bool left;
    } opens[] = {{file(1), "notes", START, true},
                 {file(1), "notes", START + DAY, false},
                 {app("a.desktop"), "ap", START + 2 * DAY, false},
                 {file(2), NULL, START + 3 * DAY, true},
                 {app("a.desktop"), "apps", START, true}};
    for (size_t i = 0; i < sizeof(opens) / sizeof(opens[0]); i++) {
        CHECK(usage_record(all, opens[i].target, opens[i].query, opens[i].at) == TL_OK);
        CHECK(usage_record(opens[i].left ? left : right, opens[i].target, opens[i].query,
                           opens[i].at) == TL_OK);
    }
    CHECK(usage_merge(left, right) == TL_OK && usage_count(left) == 3);
    const char *queries[] = {"", "n", "notes", "ap", "app"};
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++)
        for (size_t i = 0; i < sizeof(opens) / sizeof(opens[0]); i++)
            CHECK(boost(left, queries[q], START + 4 * DAY, opens[i].target) ==
                  boost(all, queries[q], START + 4 * DAY, opens[i].target));
    usage_destroy(all);
    usage_destroy(left);
    usage_destroy(right);
}
void test_usage(void) {
    contract();
    frecency();
    query_history();
    capacity();
    eviction_order();
    merge();
}
