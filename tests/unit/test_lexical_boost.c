/* Personal boosts: the side pass must return exactly what a full evaluation
 * that adds each boost to its entry's score would, on small engines (checked
 * against every match) and on a large engine using parallel scoring, skips
 * and narrowing (checked entry by entry). */
#include "test.h"
#include "torchlight/lexical.h"
#include <stdint.h>
#include <string.h>

enum { SMALL_ENTRIES = 600, LARGE_ENTRIES = 70000, LARGE_BOOSTED = 24, PATH_BYTES = 64 };
struct expected {
    uint64_t id;
    const char *path;
    int score;
};
static tl_lexical *build_engine(char (*paths)[PATH_BYTES], size_t count) {
    tl_lexical *engine = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    for (size_t i = 0; i < count; i++)
        CHECK(lexical_add(engine, i + 1, paths[i], i == 0) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    return engine;
}
/* Deterministic names with duplicates, shared folders and exact-name ties. */
static void corpus(char (*paths)[PATH_BYTES], size_t count) {
    static const char *const words[] = {"notes", "project", "photo", "apps",  "data",
                                        "main",  "readme",  "plan",  "image", "report"};
    uint64_t state = 11;
    snprintf(paths[0], PATH_BYTES, "/r");
    for (size_t i = 1; i < count; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        if (i % 50 == 0) {
            snprintf(paths[i], PATH_BYTES, "/r/%s/readme.md", words[(state >> 33) % 10]);
            continue;
        }
        snprintf(paths[i], PATH_BYTES, "/r/%s/%s%s%u.txt", words[(state >> 33) % 10],
                 words[(state >> 40) % 10], (state >> 50) % 2 ? "_" : "", (unsigned)(i % 97));
    }
}
static int boost_of(const tl_lexical *engine, const tl_lexical_boost *boosts, size_t count,
                    uint64_t id) {
    size_t slot = 0;
    CHECK(lexical_slot(engine, id, &slot) == TL_OK);
    for (size_t i = 0; i < count; i++)
        if (boosts[i].position == slot)
            return boosts[i].boost;
    return 0;
}
static int compare_expected(const void *left, const void *right) {
    const struct expected *a = left, *b = right;
    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    int order = strcmp(a->path, b->path);
    return order != 0 ? order : (a->id > b->id) - (a->id < b->id);
}
static bool blank(const char *query) {
    for (; *query != 0; query++)
        if (*query != ' ')
            return false;
    return true;
}
static void check_prefix(const tl_lexical *engine, tl_lexical_workspace *boosted, const char *query,
                         const struct expected *reference, size_t total) {
    static const size_t capacities[] = {1, 3, 10, 50};
    for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
        tl_result results[50];
        size_t count = 0, capacity = capacities[c];
        CHECK(lexical_query(engine, boosted, query, results, capacity, &count) == TL_OK);
        CHECK(count == (total < capacity ? total : capacity));
        for (size_t i = 0; i < count; i++)
            CHECK(results[i].id == reference[i].id && results[i].score == reference[i].score);
    }
}
/* Compare against every unboosted match plus its boost (complete because the
 * engine has fewer entries than LEXICAL_MAX_RESULTS). */
static void check_complete(const tl_lexical *engine, tl_lexical_workspace *plain,
                           tl_lexical_workspace *boosted, const tl_lexical_boost *boosts,
                           size_t boost_count, const char *query) {
    static tl_result all[LEXICAL_MAX_RESULTS];
    static struct expected reference[LEXICAL_MAX_RESULTS];
    size_t total = 0;
    CHECK(lexical_query(engine, plain, query, all, LEXICAL_MAX_RESULTS, &total) == TL_OK);
    CHECK(total < LEXICAL_MAX_RESULTS);
    for (size_t i = 0; i < total; i++) {
        int boost = blank(query) ? 0 : boost_of(engine, boosts, boost_count, all[i].id);
        reference[i] = (struct expected){all[i].id, all[i].path, all[i].score + boost};
    }
    qsort(reference, total, sizeof(*reference), compare_expected);
    check_prefix(engine, boosted, query, reference, total);
}
static const char *const QUERIES[] = {
    "r",           "a",          "notes",   "readme", "readme.md", "/r/plan/readme.md",
    "prj",         "photo plan", "apps rd", "noets",  "readme/x",  "r/apps",
    "data main 4", "repot",      "",        "zzz"};
static void complete_equivalence(void) {
    static char paths[SMALL_ENTRIES][PATH_BYTES];
    corpus(paths, SMALL_ENTRIES);
    tl_lexical *engine = build_engine(paths, SMALL_ENTRIES);
    tl_lexical_workspace *plain = NULL, *boosted = NULL;
    CHECK(lexical_workspace_create(engine, &plain) == TL_OK);
    CHECK(lexical_workspace_create(engine, &boosted) == TL_OK);
    static tl_lexical_boost boosts[SMALL_ENTRIES];
    size_t count = 0;
    for (size_t slot = 0; slot < SMALL_ENTRIES; slot += 3)
        boosts[count++] = (tl_lexical_boost){slot, (int)((slot * 997) % (LEXICAL_BOOST_MAX + 1))};
    CHECK(lexical_workspace_boost(boosted, boosts, count) == TL_OK);
    /* Typing forward then back keeps warm caches and narrowing in play. */
    size_t queries = sizeof(QUERIES) / sizeof(QUERIES[0]);
    for (size_t step = 0; step < 2 * queries; step++) {
        size_t q = step < queries ? step : 2 * queries - step - 1;
        check_complete(engine, plain, boosted, boosts, count, QUERIES[q]);
    }
    /* Tombstones stay absent whether or not their entries are boosted. */
    static uint64_t tombstones[SMALL_ENTRIES / 64 + 1];
    for (size_t slot = 0; slot < SMALL_ENTRIES; slot += 7)
        tombstones[slot / 64] |= UINT64_C(1) << (slot % 64);
    lexical_workspace_exclude(plain, tombstones);
    lexical_workspace_exclude(boosted, tombstones);
    for (size_t q = 0; q < queries; q++)
        check_complete(engine, plain, boosted, boosts, count, QUERIES[q]);
    lexical_workspace_destroy(plain);
    lexical_workspace_destroy(boosted);
    lexical_destroy(engine);
}
/* One-symbol queries answer from the seal-time cache, so their side pass
 * gathers evidence for the boosted entries alone: names, generic names,
 * keywords and parent folders must score exactly as a full evaluation. */
static void field_equivalence(void) {
    static const char *const names[] = {"Sound",      "Display", "Screen Reader", "Settings",
                                        "Calculator", "Shell",   "Archive",       "System Monitor"};
    static const char *const generics[] = {"Audio mixer", "Monitor setup", "Accessibility", NULL};
    static const char *const keywords[] = {"screen;resolution", "volume", NULL, "calc;math"};
    tl_lexical *engine = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_set_prefix_bonus(engine, 2000) == TL_OK);
    for (size_t i = 0; i < 160; i++) {
        char path[PATH_BYTES];
        snprintf(path, sizeof(path), "/Applications/s%zu/%s %zu", i % 7, names[i % 8], i);
        CHECK(lexical_add_fields(engine, i + 1, path, generics[i % 4], keywords[(i / 4) % 4]) ==
              TL_OK);
    }
    CHECK(lexical_finish(engine) == TL_OK);
    tl_lexical_workspace *plain = NULL, *boosted = NULL;
    CHECK(lexical_workspace_create(engine, &plain) == TL_OK);
    CHECK(lexical_workspace_create(engine, &boosted) == TL_OK);
    tl_lexical_boost boosts[80];
    size_t count = 0;
    for (size_t slot = 1; slot < 160; slot += 2)
        boosts[count++] = (tl_lexical_boost){slot, (int)((slot * 131) % (LEXICAL_BOOST_MAX + 1))};
    CHECK(lexical_workspace_boost(boosted, boosts, count) == TL_OK);
    const char *queries[] = {"s", "a", "m", "r", "v", "c", "x", "1", "s", "sc", "screen", "audio"};
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++)
        check_complete(engine, plain, boosted, boosts, count, queries[q]);
    lexical_workspace_destroy(plain);
    lexical_workspace_destroy(boosted);
    lexical_destroy(engine);
}
/* An entry just outside the head rises into it with a boost. */
static void lifting(void) {
    static char paths[SMALL_ENTRIES][PATH_BYTES];
    corpus(paths, SMALL_ENTRIES);
    tl_lexical *engine = build_engine(paths, SMALL_ENTRIES);
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    static tl_result results[LEXICAL_MAX_RESULTS];
    size_t count = 0, outside = 10;
    CHECK(lexical_query(engine, workspace, "notes", results, LEXICAL_MAX_RESULTS, &count) == TL_OK);
    while (outside < count && results[outside].score + LEXICAL_BOOST_MAX <= results[0].score)
        outside++;
    CHECK(outside < count);
    size_t slot = 0;
    CHECK(lexical_slot(engine, results[outside].id, &slot) == TL_OK);
    tl_lexical_boost lift = {slot, LEXICAL_BOOST_MAX};
    CHECK(lexical_workspace_boost(workspace, &lift, 1) == TL_OK);
    tl_result head[10];
    CHECK(lexical_query(engine, workspace, "notes", head, 10, &count) == TL_OK);
    CHECK(count == 10 && head[0].id == results[outside].id);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
/* Boosts order exact names among themselves but never lift an ordinary match
 * above one. */
static void exact_tiers(void) {
    char paths[4][PATH_BYTES] = {"/r", "/r/a/readme.md", "/r/b/readme.md", "/r/readme.md.bak"};
    tl_lexical *engine = build_engine(paths, 4);
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    tl_result results[3];
    size_t count = 0;
    CHECK(lexical_query(engine, workspace, "readme.md", results, 3, &count) == TL_OK);
    CHECK(count == 3 && results[0].id == 2 && results[1].id == 3 && results[2].id == 4);
    tl_lexical_boost boosts[2] = {{2, 1}, {3, LEXICAL_BOOST_MAX}};
    CHECK(lexical_workspace_boost(workspace, boosts, 2) == TL_OK);
    CHECK(lexical_query(engine, workspace, "readme.md", results, 3, &count) == TL_OK);
    CHECK(count == 3 && results[0].id == 3 && results[1].id == 2 && results[2].id == 4);
    CHECK(lexical_exactness(&results[0]) == LEXICAL_EXACT_NAME &&
          lexical_exactness(&results[1]) == LEXICAL_EXACT_NAME &&
          lexical_exactness(&results[2]) == LEXICAL_ORDINARY);
    /* The exact raw path keeps the top tier over a boosted exact name. */
    CHECK(lexical_query(engine, workspace, "/r/a/readme.md", results, 3, &count) == TL_OK);
    CHECK(count >= 1 && results[0].id == 2 &&
          lexical_exactness(&results[0]) == LEXICAL_EXACT_RAW_PATH);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
static void boost_contract(void) {
    char paths[4][PATH_BYTES] = {"/r", "/r/a.txt", "/r/b.txt", "/r/c.txt"};
    tl_lexical *engine = build_engine(paths, 4);
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    tl_lexical_boost boosts[3] = {{1, 5}, {2, 0}, {2, 0}};
    CHECK(lexical_workspace_boost(NULL, boosts, 1) == TL_INVALID);
    CHECK(lexical_workspace_boost(workspace, NULL, 1) == TL_INVALID);
    CHECK(lexical_workspace_boost(workspace, NULL, 0) == TL_OK);
    CHECK(lexical_workspace_boost(workspace, boosts, 3) == TL_OK); /* zero repeats ignored */
    tl_lexical_boost repeated[2] = {{1, 5}, {1, 7}};
    CHECK(lexical_workspace_boost(workspace, repeated, 2) == TL_INVALID);
    tl_lexical_boost range[2] = {{4, 1}, {0, LEXICAL_BOOST_MAX + 1}};
    CHECK(lexical_workspace_boost(workspace, &range[0], 1) == TL_INVALID);
    CHECK(lexical_workspace_boost(workspace, &range[1], 1) == TL_INVALID);
    tl_lexical_boost negative = {0, -1};
    CHECK(lexical_workspace_boost(workspace, &negative, 1) == TL_INVALID);
    CHECK(lexical_workspace_boost(workspace, boosts, LEXICAL_MAX_BOOSTED + 1) == TL_LIMIT);
    /* Every error clears the boosts attached before it. The three names score
     * equally, so lifted c.txt leads, and without its boost a.txt leads again
     * by path order. Regression: NULL boosts with a nonzero count used to
     * return before clearing. */
    const struct {
        const tl_lexical_boost *boosts;
        size_t count;
        tl_status status;
    } errors[] = {{NULL, 1, TL_INVALID},      {repeated, 2, TL_INVALID},
                  {&range[0], 1, TL_INVALID}, {&range[1], 1, TL_INVALID},
                  {&negative, 1, TL_INVALID}, {boosts, LEXICAL_MAX_BOOSTED + 1, TL_LIMIT}};
    tl_lexical_boost lift = {3, 9};
    tl_result results[3];
    size_t count = 0;
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); i++) {
        CHECK(lexical_workspace_boost(workspace, &lift, 1) == TL_OK);
        CHECK(lexical_query(engine, workspace, "txt", results, 3, &count) == TL_OK && count == 3);
        CHECK(results[0].id == 4 && results[0].score == results[1].score + 9);
        CHECK(lexical_workspace_boost(workspace, errors[i].boosts, errors[i].count) ==
              errors[i].status);
        CHECK(lexical_query(engine, workspace, "txt", results, 3, &count) == TL_OK && count == 3);
        CHECK(results[0].id == 2 && results[0].score == results[2].score);
    }
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
/* Unboosted score of one entry: rank it alone by excluding every other one.
 * Exclusion applies only when ranking, so the score equals a full query's. */
static int isolated_score(const tl_lexical *engine, tl_lexical_workspace *workspace,
                          uint64_t *excluded, size_t slot, const char *query) {
    size_t words = LARGE_ENTRIES / 64 + 1;
    memset(excluded, 0xff, words * sizeof(uint64_t));
    excluded[slot / 64] &= ~(UINT64_C(1) << (slot % 64));
    lexical_workspace_exclude(workspace, excluded);
    tl_result result[1];
    size_t count = 0;
    CHECK(lexical_query(engine, workspace, query, result, 1, &count) == TL_OK);
    lexical_workspace_exclude(workspace, NULL);
    return count == 0 ? 0 : result[0].score;
}
/* Returns how many boosted entries reached the head, so the caller can
 * check the comparison exercised the side pass. */
static size_t check_large(const tl_lexical *engine, tl_lexical_workspace *boosted,
                          tl_lexical_workspace *probe, const tl_lexical_boost *boosts,
                          uint64_t *excluded, const char *query) {
    enum { HEAD = 10 };
    static struct expected reference[HEAD + LARGE_BOOSTED];
    size_t words = LARGE_ENTRIES / 64 + 1, total = 0;
    /* Unboosted head of the other entries, then each boosted entry alone. */
    memset(excluded, 0, words * sizeof(uint64_t));
    for (size_t i = 0; i < LARGE_BOOSTED; i++)
        excluded[boosts[i].position / 64] |= UINT64_C(1) << (boosts[i].position % 64);
    lexical_workspace_exclude(probe, excluded);
    tl_result head[HEAD];
    CHECK(lexical_query(engine, probe, query, head, HEAD, &total) == TL_OK);
    lexical_workspace_exclude(probe, NULL);
    for (size_t i = 0; i < total; i++)
        reference[i] = (struct expected){head[i].id, head[i].path, head[i].score};
    for (size_t i = 0; i < LARGE_BOOSTED; i++) {
        int score = isolated_score(engine, probe, excluded, boosts[i].position, query);
        if (score == 0)
            continue;
        uint64_t id = 0;
        const char *path = NULL;
        bool is_dir = false;
        CHECK(lexical_entry(engine, boosts[i].position, &id, &path, &is_dir) == TL_OK);
        reference[total++] = (struct expected){id, path, score + boosts[i].boost};
    }
    qsort(reference, total, sizeof(*reference), compare_expected);
    tl_result results[HEAD];
    size_t count = 0;
    CHECK(lexical_query(engine, boosted, query, results, HEAD, &count) == TL_OK);
    CHECK(count == (total < HEAD ? total : HEAD));
    size_t lifted = 0;
    for (size_t i = 0; i < count; i++) {
        CHECK(results[i].id == reference[i].id && results[i].score == reference[i].score);
        size_t slot = 0;
        CHECK(lexical_slot(engine, results[i].id, &slot) == TL_OK);
        for (size_t b = 0; b < LARGE_BOOSTED; b++)
            lifted += boosts[b].position == slot;
    }
    return lifted;
}
static void large_engine(void) {
    static char paths[LARGE_ENTRIES][PATH_BYTES];
    static uint64_t excluded[LARGE_ENTRIES / 64 + 1];
    corpus(paths, LARGE_ENTRIES);
    tl_lexical *engine = build_engine(paths, LARGE_ENTRIES);
    tl_lexical_workspace *boosted = NULL, *probe = NULL;
    CHECK(lexical_workspace_create(engine, &boosted) == TL_OK);
    CHECK(lexical_workspace_create(engine, &probe) == TL_OK);
    tl_lexical_boost boosts[LARGE_BOOSTED];
    for (size_t i = 0; i < LARGE_BOOSTED; i++)
        boosts[i] =
            (tl_lexical_boost){1 + i * 2903, (int)(64 + (i * 389) % (LEXICAL_BOOST_MAX - 64))};
    CHECK(lexical_workspace_boost(boosted, boosts, LARGE_BOOSTED) == TL_OK);
    const char *queries[] = {"r", "notes", "readme.md", "photo plan", "noets", "r/apps", "prj"};
    size_t lifted = 0;
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++)
        lifted += check_large(engine, boosted, probe, boosts, excluded, queries[q]);
    CHECK(lifted != 0);
    lexical_workspace_destroy(boosted);
    lexical_workspace_destroy(probe);
    lexical_destroy(engine);
}
/* Selective queries on a large engine: the main pass scans every entry and
 * resolves every directory, so a big side batch is split across workers.
 * With few matches the complete reference applies. */
static void large_parallel_side(void) {
    enum { PARALLEL_BOOSTED = 1500 };
    static char paths[LARGE_ENTRIES][PATH_BYTES];
    static tl_lexical_boost boosts[PARALLEL_BOOSTED];
    corpus(paths, LARGE_ENTRIES);
    tl_lexical *engine = build_engine(paths, LARGE_ENTRIES);
    tl_lexical_workspace *plain = NULL, *boosted = NULL;
    CHECK(lexical_workspace_create(engine, &plain) == TL_OK);
    CHECK(lexical_workspace_create(engine, &boosted) == TL_OK);
    for (size_t i = 0; i < PARALLEL_BOOSTED; i++)
        boosts[i] = (tl_lexical_boost){1 + i * 46, (int)(1 + (i * 389) % LEXICAL_BOOST_MAX)};
    CHECK(lexical_workspace_boost(boosted, boosts, PARALLEL_BOOSTED) == TL_OK);
    /* Abbreviations have no strong channel hits, so the main pass scans; the
     * multiword and path queries take the other side-pass routes. */
    const char *queries[] = {"nts96",        "rprt42",       "pht33",          "readme/plan",
                             "data photo_1", "plan/notes_5", "image report_42"};
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++)
        check_complete(engine, plain, boosted, boosts, PARALLEL_BOOSTED, queries[q]);
    lexical_workspace_destroy(plain);
    lexical_workspace_destroy(boosted);
    lexical_destroy(engine);
}
void test_lexical_boost(void) {
    large_parallel_side();
    boost_contract();
    lifting();
    exact_tiers();
    complete_equivalence();
    field_equivalence();
    large_engine();
}
