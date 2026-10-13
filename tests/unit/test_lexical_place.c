/* Place ranking: shallow and own files before deep and dependency ones at
 * equal match strength, without crossing channel tiers; results equal a
 * brute-force ranking (place-off scores minus each entry's penalty) at every
 * capacity, with boosts and tombstones, on a small engine and on a large one
 * that scores in parallel; a delta segment scores like a full rebuild. */
#include "test.h"
#include "torchlight/lexical.h"
#include "torchlight/place.h"
#include <stdint.h>
#include <string.h>

enum { SMALL_ENTRIES = 700, LARGE_ENTRIES = 70000, PATH_BYTES = 160, MAX_DEPTH = 9 };
struct expected {
    uint64_t id;
    const char *path;
    int score;
};
static tl_lexical *build(const char *const *paths, size_t count, bool place) {
    tl_lexical *engine = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_set_place_ranking(engine, place) == TL_OK);
    for (size_t i = 0; i < count; i++)
        CHECK(lexical_add(engine, i + 1, paths[i], i == 0) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    return engine;
}
static size_t query(const tl_lexical *engine, const char *text, tl_result *results,
                    size_t capacity) {
    tl_lexical_workspace *workspace = NULL;
    size_t count = 0;
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    CHECK(lexical_query(engine, workspace, text, results, capacity, &count) == TL_OK);
    lexical_workspace_destroy(workspace);
    return count;
}
/* Penalty of an absolute path below the single root /r, from the place
 * module directly: every directory from /r down to the parent is usable. */
static int penalty_of(const char *path) {
    tl_place place = {0};
    const char *start = path + 1, *slash = NULL;
    while ((slash = strchr(start, '/')) != NULL) {
        char name[PATH_BYTES];
        size_t length = (size_t)(slash - start);
        memcpy(name, start, length);
        name[length] = 0;
        tl_tokenized *text = test_text(name);
        place = place_child(place, tokenize_view(text), true);
        tokenize_destroy(text);
        start = slash + 1;
    }
    return place_penalty(place);
}
/* ---- targeted ordering ------------------------------------------------- */
static const char *const ORDERING[] = {
    "/r",
    "/r/a/b/c/d/e/f/scan",                  /* depth 6: three levels beyond free */
    "/r/work/scripts",                      /* shallow, longer name */
    "/r/lib/site-packages",                 /* the marker folder itself */
    "/r/lib/site-packages/scripts",         /* inside a dependency tree */
    "/r/node_modules/a/b/c/d/e/f/g/report", /* capped penalty */
    "/r/my_report.txt",                     /* shallow token prefix */
};
static int rank_of(const tl_result *results, size_t count, const char *path) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(results[i].path, path) == 0)
            return (int)i;
    return -1;
}
static void ordering(void) {
    size_t entries = sizeof(ORDERING) / sizeof(ORDERING[0]);
    tl_lexical *off = build(ORDERING, entries, false), *on = build(ORDERING, entries, true);
    tl_result results[10];
    /* Without place, the shorter deep name wins; with it, the shallow one. */
    size_t count = query(off, "sc", results, 10);
    CHECK(rank_of(results, count, "/r/a/b/c/d/e/f/scan") == 0);
    count = query(on, "sc", results, 10);
    CHECK(rank_of(results, count, "/r/work/scripts") == 0);
    CHECK(rank_of(results, count, "/r/a/b/c/d/e/f/scan") == 1);
    CHECK(rank_of(results, count, "/r/lib/site-packages/scripts") == 2);
    /* Exact names keep their tier, ordered by place. */
    count = query(on, "scripts", results, 10);
    CHECK(count >= 2 && strcmp(results[0].path, "/r/work/scripts") == 0);
    CHECK(strcmp(results[1].path, "/r/lib/site-packages/scripts") == 0);
    CHECK(lexical_exactness(&results[0]) == LEXICAL_EXACT_NAME);
    CHECK(lexical_exactness(&results[1]) == LEXICAL_EXACT_NAME);
    CHECK(results[0].score - results[1].score == PLACE_DEPENDENCY_PENALTY);
    /* The marker folder is not inside its own tree. */
    count = query(on, "site-packages", results, 10);
    CHECK(count >= 1 && strcmp(results[0].path, "/r/lib/site-packages") == 0);
    CHECK(lexical_exactness(&results[0]) == LEXICAL_EXACT_NAME);
    tl_result plain[10];
    CHECK(query(off, "site-packages", plain, 10) >= 1 && plain[0].score == results[0].score);
    /* A capped basename prefix still beats a shallow token prefix. */
    count = query(on, "report", results, 10);
    CHECK(rank_of(results, count, "/r/node_modules/a/b/c/d/e/f/g/report") == 0);
    CHECK(rank_of(results, count, "/r/my_report.txt") == 1);
    /* Exact raw paths and roots are not penalized. */
    CHECK(query(on, "/r/a/b/c/d/e/f/scan", results, 10) >= 1);
    CHECK(lexical_exactness(&results[0]) == LEXICAL_EXACT_RAW_PATH);
    CHECK(query(off, "/r/a/b/c/d/e/f/scan", plain, 10) >= 1 && plain[0].score == results[0].score);
    CHECK(query(on, "", results, 10) == 1 && query(off, "", plain, 10) == 1);
    CHECK(results[0].score == plain[0].score);
    lexical_destroy(off);
    lexical_destroy(on);
}
/* A single-word query may skip its scan once capacity channel hits beat the
 * best possible unhit score. That bound is unpenalized, so a hit only counts
 * by its penalized score. Here a deep dependency-tree entry's complete keyword
 * hit (2728 + 54 short-name bonus - 224 = 2558) falls below the 2564 bound,
 * and an unhit file whose parent folder starts with the word (2500 + 63) must
 * win the single result slot. Counting the hit unpenalized skips that scan. */
static void skip_counts_penalized_hits(void) {
    tl_lexical *engine = NULL;
    CHECK(lexical_create(&engine) == TL_OK && lexical_set_place_ranking(engine, true) == TL_OK);
    CHECK(lexical_add(engine, 1, "/r", true) == TL_OK);
    CHECK(lexical_add_fields(engine, 2, "/r/node_modules/a/b/c/d/e/f/aaaaaaaaaa", NULL, "wxyz") ==
          TL_OK);
    CHECK(lexical_add(engine, 3, "/r/wxyzdir/x", false) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    tl_result results[2];
    CHECK(query(engine, "wxyz", results, 1) == 1 && results[0].id == 3);
    CHECK(query(engine, "wxyz", results, 2) == 2 && results[0].id == 3 && results[1].id == 2);
    lexical_destroy(engine);
}
static void contracts(void) {
    tl_lexical *engine = NULL;
    CHECK(lexical_set_place_ranking(NULL, true) == TL_INVALID);
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_set_place_ranking(engine, true) == TL_OK);
    CHECK(lexical_add(engine, 1, "/r", true) == TL_OK && lexical_finish(engine) == TL_OK);
    CHECK(lexical_set_place_ranking(engine, false) == TL_STATE);
    lexical_destroy(engine);
}
/* ---- brute-force equivalence ------------------------------------------- */
static const char *const DIRS[] = {"work", "notes",  "site-packages", "vendor",  "1.2.3", "x@v1.0",
                                   "sdk",  "photos", "src",           "Android", "plan",  "data"};
static const char *const NAMES[] = {"scan",   "scripts",  "scan.c",  "readme",      "readme.md",
                                    "notes",  "plan.txt", "sched",   "report_2024", "Scene.h",
                                    "vendor", "sdk",      "main.go", "screen",      "my notes"};
static uint64_t next(uint64_t *state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state >> 33;
}
/* Paths below /r at depths 0..MAX_DEPTH, with repeated names so that many
 * entries tie on their match and differ only by place. */
static void corpus(char (*paths)[PATH_BYTES], size_t count, uint64_t seed) {
    uint64_t state = seed;
    snprintf(paths[0], PATH_BYTES, "/r");
    for (size_t i = 1; i < count; i++) {
        size_t depth = next(&state) % (MAX_DEPTH + 1), used = 2;
        memcpy(paths[i], "/r", 3);
        for (size_t d = 0; d < depth; d++)
            used += (size_t)snprintf(paths[i] + used, PATH_BYTES - used, "/%s",
                                     DIRS[next(&state) % (sizeof(DIRS) / sizeof(DIRS[0]))]);
        snprintf(paths[i] + used, PATH_BYTES - used, "/%s%u",
                 NAMES[next(&state) % (sizeof(NAMES) / sizeof(NAMES[0]))],
                 (unsigned)(next(&state) % 3 == 0 ? i % 7 : 0));
        /* Suffix 0 is dropped so exact names repeat. */
        size_t length = strlen(paths[i]);
        if (paths[i][length - 1] == '0')
            paths[i][length - 1] = 0;
    }
}
static int compare_expected(const void *left, const void *right) {
    const struct expected *a = left, *b = right;
    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    int order = strcmp(a->path, b->path);
    return order != 0 ? order : (a->id > b->id) - (a->id < b->id);
}
/* The place-off result with its penalty taken, as lexical_set_place_ranking
 * documents: ordinary scores stay at least 1, exact paths and roots keep theirs. */
static int penalized(const tl_result *result, bool blank) {
    if (blank || lexical_exactness(result) == LEXICAL_EXACT_RAW_PATH)
        return result->score;
    int score = result->score - penalty_of(result->path);
    if (lexical_exactness(result) == LEXICAL_EXACT_NAME)
        return score;
    return score < 1 ? 1 : score;
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
struct pair {
    const tl_lexical *off, *on;
    tl_lexical_workspace *plain, *ranked;
    const tl_lexical_boost *boosts;
    size_t boost_count;
};
/* Every match of the place-off engine (complete: fewer than the result
 * capacity), penalized and boosted, sorted by the engines' total order. */
static size_t reference(const struct pair *pair, const char *text, struct expected *out) {
    static tl_result all[LEXICAL_MAX_RESULTS];
    size_t total = 0;
    CHECK(lexical_query(pair->off, pair->plain, text, all, LEXICAL_MAX_RESULTS, &total) == TL_OK);
    CHECK(total < LEXICAL_MAX_RESULTS);
    bool blank = text[strspn(text, " ")] == 0;
    for (size_t i = 0; i < total; i++) {
        int boost = blank ? 0 : boost_of(pair->on, pair->boosts, pair->boost_count, all[i].id);
        out[i] = (struct expected){all[i].id, all[i].path, penalized(&all[i], blank) + boost};
    }
    qsort(out, total, sizeof(*out), compare_expected);
    return total;
}
static void check_query(const struct pair *pair, const char *text) {
    static struct expected expected[LEXICAL_MAX_RESULTS];
    static const size_t capacities[] = {1, 5, 10, 200};
    size_t total = reference(pair, text, expected);
    for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
        static tl_result results[200];
        size_t count = 0, capacity = capacities[c];
        CHECK(lexical_query(pair->on, pair->ranked, text, results, capacity, &count) == TL_OK);
        CHECK(count == (total < capacity ? total : capacity));
        for (size_t i = 0; i < count; i++)
            CHECK(results[i].id == expected[i].id && results[i].score == expected[i].score);
    }
}
static const char *const QUERIES[] = {
    "s",        "sc",     "scan", "scripts", "readme", "readme.md", "notes plan",
    "sdk",      "vendor", "r/sc", "scna",    "rdm",    "work scan", "site-packages",
    "my notes", "",       "zzz"};
static void run_queries(const struct pair *pair, char (*paths)[PATH_BYTES], size_t entries) {
    size_t queries = sizeof(QUERIES) / sizeof(QUERIES[0]);
    /* Forward then back keeps warm caches and narrowing in play. */
    for (size_t step = 0; step < 2 * queries; step++)
        check_query(pair, QUERIES[step < queries ? step : 2 * queries - step - 1]);
    /* An exact raw path of a deep entry. */
    for (size_t i = entries - 1; i > 0; i--) {
        if (strchr(paths[i] + 3, '/') != NULL) {
            check_query(pair, paths[i]);
            break;
        }
    }
}
static void small_equivalence(void) {
    static char paths[SMALL_ENTRIES][PATH_BYTES];
    static const char *views[SMALL_ENTRIES];
    corpus(paths, SMALL_ENTRIES, 17);
    for (size_t i = 0; i < SMALL_ENTRIES; i++)
        views[i] = paths[i];
    struct pair pair = {.off = build(views, SMALL_ENTRIES, false),
                        .on = build(views, SMALL_ENTRIES, true)};
    CHECK(lexical_workspace_create(pair.off, &pair.plain) == TL_OK);
    CHECK(lexical_workspace_create(pair.on, &pair.ranked) == TL_OK);
    run_queries(&pair, paths, SMALL_ENTRIES);
    /* Boosts add on top of the penalty; tombstones stay absent. */
    static tl_lexical_boost boosts[SMALL_ENTRIES];
    size_t count = 0;
    for (size_t slot = 0; slot < SMALL_ENTRIES; slot += 5)
        boosts[count++] = (tl_lexical_boost){slot, (int)((slot * 389) % (LEXICAL_BOOST_MAX + 1))};
    CHECK(lexical_workspace_boost(pair.ranked, boosts, count) == TL_OK);
    pair.boosts = boosts;
    pair.boost_count = count;
    static uint64_t tombstones[SMALL_ENTRIES / 64 + 1];
    for (size_t slot = 3; slot < SMALL_ENTRIES; slot += 11)
        tombstones[slot / 64] |= UINT64_C(1) << (slot % 64);
    lexical_workspace_exclude(pair.plain, tombstones);
    lexical_workspace_exclude(pair.ranked, tombstones);
    run_queries(&pair, paths, SMALL_ENTRIES);
    lexical_workspace_destroy(pair.plain);
    lexical_workspace_destroy(pair.ranked);
    lexical_destroy((tl_lexical *)pair.off);
    lexical_destroy((tl_lexical *)pair.on);
}
/* Above LEXICAL_PARALLEL_ENGINE_MIN entries, scoring runs in parallel batches
 * and single-word queries can skip the scan. Filler names never match the
 * queries, so each query's matches stay below the result capacity and the
 * reference remains complete. */
static void large_equivalence(void) {
    static char paths[LARGE_ENTRIES][PATH_BYTES];
    static const char *views[LARGE_ENTRIES];
    corpus(paths, 600, 29);
    for (size_t i = 600; i < LARGE_ENTRIES; i++)
        snprintf(paths[i], PATH_BYTES, "/r/fill/%s/kx%zu", i % 3 ? "deep/er/than/free" : "f", i);
    for (size_t i = 0; i < LARGE_ENTRIES; i++)
        views[i] = paths[i];
    struct pair pair = {.off = build(views, LARGE_ENTRIES, false),
                        .on = build(views, LARGE_ENTRIES, true)};
    CHECK(lexical_workspace_create(pair.off, &pair.plain) == TL_OK);
    CHECK(lexical_workspace_create(pair.on, &pair.ranked) == TL_OK);
    const char *queries[] = {"sc", "scan", "scripts", "readme", "notes plan", "scna", "rdm", "sdk"};
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++)
        check_query(&pair, queries[q]);
    lexical_workspace_destroy(pair.plain);
    lexical_workspace_destroy(pair.ranked);
    lexical_destroy((tl_lexical *)pair.off);
    lexical_destroy((tl_lexical *)pair.on);
}
/* A delta over base, with place ranking inherited from it. */
static tl_lexical *delta_over(const tl_lexical *base, char (*paths)[PATH_BYTES], size_t from,
                              size_t to) {
    tl_lexical *delta = NULL;
    CHECK(lexical_create(&delta) == TL_OK && lexical_set_reference(delta, base) == TL_OK);
    for (size_t i = from; i < to; i++)
        CHECK(lexical_add(delta, i + 1, paths[i], false) == TL_OK);
    CHECK(lexical_finish(delta) == TL_OK);
    return delta;
}
static const tl_result *find(const tl_result *results, size_t count, uint64_t id) {
    for (size_t i = 0; i < count; i++)
        if (results[i].id == id)
            return &results[i];
    return NULL;
}
/* A delta segment inherits place ranking from its reference and penalizes
 * each entry exactly as a full rebuild does. Its trigram channel takes
 * frequencies from the base, so a few weak trigram matches differ from a
 * rebuild with or without place ranking; compare only entries whose
 * place-off scores agree, and check every delta result against its own
 * place-off score. */
static void delta_matches_rebuild(void) {
    enum { BASE = 400, ALL = 520 };
    static char paths[ALL][PATH_BYTES];
    static const char *views[ALL];
    corpus(paths, ALL, 41);
    for (size_t i = 0; i < ALL; i++)
        views[i] = paths[i];
    tl_lexical *engines[] = {build(views, BASE, true), build(views, BASE, false),
                             build(views, ALL, true), build(views, ALL, false)};
    tl_lexical *delta_on = delta_over(engines[0], paths, BASE, ALL);
    tl_lexical *delta_off = delta_over(engines[1], paths, BASE, ALL);
    static tl_result on[LEXICAL_MAX_RESULTS], off[LEXICAL_MAX_RESULTS];
    static tl_result full_on[LEXICAL_MAX_RESULTS], full_off[LEXICAL_MAX_RESULTS];
    for (size_t q = 0; q < sizeof(QUERIES) / sizeof(QUERIES[0]); q++) {
        bool blank = QUERIES[q][0] == 0;
        size_t count = query(delta_on, QUERIES[q], on, LEXICAL_MAX_RESULTS);
        CHECK(query(delta_off, QUERIES[q], off, LEXICAL_MAX_RESULTS) == count);
        size_t total = query(engines[2], QUERIES[q], full_on, LEXICAL_MAX_RESULTS);
        CHECK(query(engines[3], QUERIES[q], full_off, LEXICAL_MAX_RESULTS) == total);
        for (size_t i = 0; i < count; i++) {
            const tl_result *plain = find(off, count, on[i].id);
            CHECK(plain != NULL && penalized(plain, blank) == on[i].score);
            const tl_result *rebuilt = find(full_off, total, on[i].id);
            if (rebuilt != NULL && rebuilt->score == plain->score)
                CHECK(find(full_on, total, on[i].id)->score == on[i].score);
        }
    }
    lexical_destroy(delta_on);
    lexical_destroy(delta_off);
    for (size_t i = 0; i < sizeof(engines) / sizeof(engines[0]); i++)
        lexical_destroy(engines[i]);
}
void test_lexical_place(void) {
    contracts();
    ordering();
    skip_counts_penalized_hits();
    small_equivalence();
    large_equivalence();
    delta_matches_rebuild();
}
