/* Complete scans, deterministic top-k, query edits and byte-safe exact paths. */
#include "test.h"
#include "torchlight/lexical.h"
#include <stdio.h>
#include <string.h>
static void expect(tl_lexical *engine, tl_lexical_workspace *workspace, const char *query,
                   const char *path) {
    tl_result results[10];
    size_t count = 0;
    CHECK(lexical_query(engine, workspace, query, results, 10, &count) == TL_OK);
    CHECK(count > 0 && strcmp(results[0].path, path) == 0);
}
static void complete_scan_regression(void) {
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_add(engine, 1, "/a/alpha.txt", false) == TL_OK);
    CHECK(lexical_add(engine, 2, "/z/alphaZebra.txt", false) == TL_OK);
    const char *resolved = NULL;
    CHECK(lexical_resolve(engine, 1, &resolved) == TL_STATE && resolved == NULL);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(lexical_resolve(engine, 1, &resolved) == TL_OK && strcmp(resolved, "/a/alpha.txt") == 0);
    CHECK(lexical_resolve(engine, 2, &resolved) == TL_OK &&
          strcmp(resolved, "/z/alphaZebra.txt") == 0);
    CHECK(lexical_resolve(engine, 3, &resolved) == TL_STATE && resolved == NULL);
    CHECK(lexical_resolve(engine, 0, &resolved) == TL_INVALID && resolved == NULL);
    CHECK(lexical_resolve(engine, UINT64_MAX, &resolved) == TL_STATE && resolved == NULL);
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    tl_result result[1];
    size_t count = 0;
    CHECK(lexical_query(engine, workspace, "a", result, 1, &count) == TL_OK);
    CHECK(count == 1 && result[0].id == 1);
    CHECK(lexical_query(engine, workspace, "az", result, 1, &count) == TL_OK);
    CHECK(count == 1 && result[0].id == 2);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(lexical_resolve(engine, 1, &resolved) == TL_STATE && resolved == NULL);
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    CHECK(lexical_query(engine, workspace, "abc", result, 1, &count) == TL_OK && count == 0);
    CHECK(lexical_query(engine, workspace, "", result, 1, &count) == TL_OK && count == 0);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
static size_t match_count(const tl_lexical *engine, tl_lexical_workspace *workspace,
                          const char *query) {
    tl_result results[10];
    size_t count = 0;
    CHECK(lexical_query(engine, workspace, query, results, 10, &count) == TL_OK);
    return count;
}
/* Regression: parent subsequences spanned the whole absolute path, so short
 * queries like "hp" matched letters scattered across /home/user/... and
 * returned nearly every entry. Parent matches now stay within one directory
 * name unless the word itself contains '/'. */
static void parent_matches_stay_in_one_component(void) {
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_add(engine, 1, "/home/user/p.txt", false) == TL_OK);
    CHECK(lexical_add(engine, 2, "/srv/projectNotes/x.txt", false) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    CHECK(match_count(engine, workspace, "hp") == 0);
    CHECK(match_count(engine, workspace, "hmusr") == 0);
    expect(engine, workspace, "prjnts", "/srv/projectNotes/x.txt");
    expect(engine, workspace, "hm/p", "/home/user/p.txt");
    CHECK(match_count(engine, workspace, "/") == 2);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
/* Build a sealed engine over paths (ids 1..count; paths[0] is the root). */
static tl_lexical *build(const char *const *paths, size_t count) {
    tl_lexical *engine = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    for (size_t i = 0; i < count; i++)
        CHECK(lexical_add(engine, i + 1, paths[i], i == 0) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    return engine;
}
static const char *const CORPUS[] = {"/home/user",
                                     "/home/user/docs",
                                     "/home/user/docs/README.md",
                                     "/home/user/docs/readme-old.txt",
                                     "/home/user/work/projectNotes.md",
                                     "/home/user/work/project_plan.txt",
                                     "/home/user/finance",
                                     "/home/user/finance/invoice2024.pdf",
                                     "/home/user/misc/finalInvoice.js",
                                     "/home/user/misc/fixture_insurance.txt",
                                     "/home/user/photos/IMG_0001.jpg",
                                     "/home/user/photos/IMG_0002.jpg",
                                     "/home/user/photos/holiday/beach.png",
                                     "/home/user/src/HTMLParser.c",
                                     "/home/user/src/main.c",
                                     "/home/user/src/report2024.pdf"};
enum { CORPUS_SIZE = sizeof(CORPUS) / sizeof(CORPUS[0]) };
static void typo_and_trigram_channels(void) {
    tl_lexical *engine = build(CORPUS, CORPUS_SIZE);
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    expect(engine, workspace, "raedme", "/home/user/docs/README.md");
    expect(engine, workspace, "projectntoes", "/home/user/work/projectNotes.md");
    expect(engine, workspace, "hp", "/home/user/src/HTMLParser.c"); /* acronym initials */
    expect(engine, workspace, "report2024", "/home/user/src/report2024.pdf");
    /* A folder named like the word beats letters scattered through a name. */
    expect(engine, workspace, "finance invoice", "/home/user/finance/invoice2024.pdf");
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
/* Directories above the indexed root ("/home") are not parent context. */
static void root_ancestors_are_not_context(void) {
    tl_lexical *engine = build(CORPUS, CORPUS_SIZE);
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    CHECK(match_count(engine, workspace, "home") == 0);
    /* The root's own name stays usable context; the root itself ranks first. */
    expect(engine, workspace, "user", "/home/user");
    expect(engine, workspace, "holiday beach", "/home/user/photos/holiday/beach.png");
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
/* Small capacities let strong hits skip scans and drop deferred parent-only
 * matches; results must still be the head of the full ranking. */
static void capacity_consistency(const tl_lexical *engine, tl_lexical_workspace *workspace,
                                 const char *query) {
    static tl_result all[LEXICAL_MAX_RESULTS];
    tl_result few[3];
    size_t all_count = 0, few_count = 0;
    CHECK(lexical_query(engine, workspace, query, all, LEXICAL_MAX_RESULTS, &all_count) == TL_OK);
    CHECK(lexical_query(engine, workspace, query, few, 3, &few_count) == TL_OK);
    CHECK(few_count == (all_count < 3 ? all_count : 3));
    for (size_t i = 0; i < few_count; i++)
        CHECK(few[i].id == all[i].id && few[i].score == all[i].score);
}
/* A complete first token must beat a shorter name containing only its prefix,
 * including separator, camelCase, acronym and cached one-symbol boundaries. */
static void first_token_completeness(void) {
    const struct {
        const char *complete, *partial, *query;
    } cases[] = {{"/work/project notes.txt", "/work/projectile.txt", "project"},
                 {"/work/projectNotes.md", "/work/projectile.md", "project"},
                 {"/work/project_notes.txt", "/work/projectile.txt", "PROJECT"},
                 {"/work/HTMLParserGuide.c", "/work/htmlish.c", "html"},
                 {"/work/a_long_description.txt", "/work/aardvark.txt", "a"}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *paths[] = {"/work", cases[i].complete, cases[i].partial};
        tl_lexical *engine = build(paths, sizeof(paths) / sizeof(paths[0]));
        tl_lexical_workspace *workspace = NULL;
        CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
        tl_result result[1];
        size_t count = 0;
        CHECK(lexical_query(engine, workspace, cases[i].query, result, 1, &count) == TL_OK);
        CHECK(count == 1 && result[0].id == 2);
        expect(engine, workspace, cases[i].query, cases[i].complete);
        capacity_consistency(engine, workspace, cases[i].query);
        expect(engine, workspace, strrchr(cases[i].partial, '/') + 1, cases[i].partial);
        lexical_workspace_destroy(workspace);
        lexical_destroy(engine);
    }
}
/* A caller's name-prefix weighting must preserve exact priorities and the
 * complete ranking through cached one-symbol and multiword bound shortcuts. */
static void prefix_bonus_ranking(void) {
    const char *paths[] = {"/r",        "/r/Google Chrome",       "/r/chromepolicy.txt",
                           "/r/chrome", "/r/google/notes chrome", "/r/chrome/browser google notes"};
    CHECK(lexical_set_prefix_bonus(NULL, 0) == TL_INVALID);
    const int bonuses[] = {0, 2000, LEXICAL_PREFIX_BONUS_MAX};
    for (size_t b = 0; b < sizeof(bonuses) / sizeof(bonuses[0]); b++) {
        tl_lexical *engine = NULL;
        tl_lexical_workspace *workspace = NULL;
        CHECK(lexical_create(&engine) == TL_OK);
        CHECK(lexical_set_prefix_bonus(engine, bonuses[b]) == TL_OK);
        CHECK(lexical_set_prefix_bonus(engine, -1) == TL_INVALID);
        CHECK(lexical_set_prefix_bonus(engine, LEXICAL_PREFIX_BONUS_MAX + 1) == TL_INVALID);
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
            CHECK(lexical_add(engine, i + 1, paths[i], i == 0) == TL_OK);
        CHECK(lexical_finish(engine) == TL_OK);
        CHECK(lexical_set_prefix_bonus(engine, 0) == TL_STATE);
        CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
        expect(engine, workspace, "chrome", paths[3]);
        expect(engine, workspace, "Google Chrome", paths[1]);
        expect(engine, workspace, paths[2], paths[2]);
        const char *queries[] = {"c",
                                 "ch",
                                 "chr",
                                 "chrome",
                                 "chr",
                                 "google chr",
                                 "google chrome notes",
                                 "notes google chrome",
                                 "browser notes"};
        for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++)
            capacity_consistency(engine, workspace, queries[i]);
        tl_result results[10];
        size_t count = 0;
        CHECK(lexical_query(engine, workspace, "chr", results, 10, &count) == TL_OK);
        CHECK(count > 0 && results[0].score == 6000 + bonuses[b] + 58);
        lexical_workspace_destroy(workspace);
        lexical_destroy(engine);
    }
}
/* Multiword channel bounds must retain weak parent matches when necessary,
 * preserve score ties, and seed exact names with separators inside a word. */
static void multiword_bounds(void) {
    const char *paths[] = {"/r",
                           "/r/a/x-alpha_beta-y.txt",
                           "/r/z/x alpha_beta y",
                           "/r/q/alpha_beta x y.old",
                           "/r/a/alpha_betagamma.txt",
                           "/r/z/Alpha Beta.txt",
                           "/r/alpha/beta.txt",
                           "/r/beta/alpha.txt"};
    tl_lexical *engine = build(paths, sizeof(paths) / sizeof(paths[0]));
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    tl_result result[1];
    size_t count = 0;
    CHECK(lexical_query(engine, workspace, "x alpha_beta y", result, 1, &count) == TL_OK);
    CHECK(count == 1 && result[0].id == 3 && result[0].score == 10000000);
    const char *queries[] = {"alpha beta",     "alpha betagamma", "alpha be",      "beta alpha",
                             "x alpha_beta y", "alpha /r/beta",   "zznomatch beta"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++)
        capacity_consistency(engine, workspace, queries[i]);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
/* More words than evidence slots must evict safely. Reordering, duplicate
 * words and a changed word also exercise the query epoch and parent context. */
static void evidence_eviction(void) {
    const char *paths[] = {"/r", "/r/one/two/three/four/five/six.txt", "/r/unmatched/six.txt",
                           "/r/one/two/threex/four/five/six.txt"};
    tl_lexical *engine = build(paths, sizeof(paths) / sizeof(paths[0]));
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    const struct {
        const char *query;
        size_t count;
        uint64_t id;
        int score;
    } cases[] = {{"one two three four five six", 2, 2, 18685},
                 {"six five four three two one", 2, 2, 18685},
                 {"one two threx four five six", 1, 4, 16624},
                 {"one one two three four five six", 2, 2, 21185},
                 {"one two missing four five six", 0, 0, 0}};
    for (size_t q = 0; q < sizeof(cases) / sizeof(cases[0]); q++) {
        tl_result results[10];
        size_t count = 0;
        CHECK(lexical_query(engine, workspace, cases[q].query, results, 10, &count) == TL_OK);
        CHECK(count == cases[q].count);
        if (count != 0)
            CHECK(results[0].id == cases[q].id && results[0].score == cases[q].score);
        capacity_consistency(engine, workspace, cases[q].query);
    }
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
/* Large fixed-width names have an analytic ranking: every match has the same
 * score and byte-path order equals id order. This exercises worker batches,
 * parent-only scans, typo hits and warm membership without a second scorer. */
static void parallel_ranking(void) {
    enum { PARALLEL_CORPUS_FILES = 65536 };
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_add(engine, 1, "/r", true) == TL_OK);
    for (size_t i = 0; i < PARALLEL_CORPUS_FILES; i++) {
        char path[64];
        int length = snprintf(path, sizeof(path), "/r/mad_notes_%06zu.txt", i);
        CHECK(length > 0 && (size_t)length < sizeof(path));
        CHECK(lexical_add(engine, i + 2, path, false) == TL_OK);
    }
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    const struct {
        const char *query;
        int score;
    } cases[] = {{"md", 1863}, {"r md", 4363}, {"mad notes", 11300}, {"r noets", 6044}};
    for (size_t q = 0; q < sizeof(cases) / sizeof(cases[0]); q++) {
        tl_result results[10];
        size_t count = 0;
        CHECK(lexical_query(engine, workspace, cases[q].query, results, 10, &count) == TL_OK);
        CHECK(count == 10);
        for (size_t i = 0; i < count; i++)
            CHECK(results[i].id == i + 2 && results[i].score == cases[q].score);
        capacity_consistency(engine, workspace, cases[q].query);
    }
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
/* A deterministic pseudo-random corpus large enough for skips and caches. */
static tl_lexical *random_engine(char (*paths)[64], size_t count) {
    static const char *const words[] = {"notes", "project", "photo", "apps",  "data",
                                        "main",  "readme",  "plan",  "image", "report"};
    uint64_t state = 7;
    const char *pointers[512];
    snprintf(paths[0], 64, "/r");
    pointers[0] = paths[0];
    for (size_t i = 1; i < count; i++) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        snprintf(paths[i], 64, "/r/%s/%s%s%u.txt", words[(state >> 33) % 10],
                 words[(state >> 40) % 10], (state >> 50) % 2 ? "_" : "", (unsigned)(i % 97));
        pointers[i] = paths[i];
    }
    return build(pointers, count);
}
/* Typing, backspacing and multiword sequences on a warm workspace (narrowing
 * caches, symbol cache, skips) must equal a fresh workspace every step. */
static void warm_equals_fresh(void) {
    static char paths[512][64];
    tl_lexical *engine = random_engine(paths, 512);
    tl_lexical_workspace *warm = NULL;
    CHECK(lexical_workspace_create(engine, &warm) == TL_OK);
    const char *typed[] = {"projectnotes", "photo plan", "apps rd", "noets", "readme/x", "r/apps"};
    for (size_t q = 0; q < sizeof(typed) / sizeof(typed[0]); q++) {
        size_t length = strlen(typed[q]);
        for (size_t step = 1; step <= 2 * length; step++) {
            size_t size = step <= length ? step : 2 * length - step + 1; /* type, then erase */
            char prefix[64];
            memcpy(prefix, typed[q], size);
            prefix[size] = 0;
            tl_lexical_workspace *fresh = NULL;
            CHECK(lexical_workspace_create(engine, &fresh) == TL_OK);
            tl_result a[10], b[10];
            size_t na = 0, nb = 0;
            CHECK(lexical_query(engine, warm, prefix, a, 10, &na) == TL_OK);
            CHECK(lexical_query(engine, fresh, prefix, b, 10, &nb) == TL_OK);
            CHECK(na == nb);
            for (size_t i = 0; i < na; i++)
                CHECK(a[i].id == b[i].id && a[i].score == b[i].score);
            lexical_workspace_destroy(fresh);
            capacity_consistency(engine, warm, prefix);
        }
    }
    /* One-symbol answers come from the seal-time cache, in any case/spacing. */
    tl_result upper[5], lower[5];
    size_t nu = 0, nl = 0;
    CHECK(lexical_query(engine, warm, " P ", upper, 5, &nu) == TL_OK);
    CHECK(lexical_query(engine, warm, "p", lower, 5, &nl) == TL_OK);
    CHECK(nu == nl && nl == 5);
    for (size_t i = 0; i < nl; i++)
        CHECK(upper[i].id == lower[i].id);
    capacity_consistency(engine, warm, "p");
    lexical_workspace_destroy(warm);
    lexical_destroy(engine);
}
static void quality_fields(void) {
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_add_fields(engine, 1, "/Applications/Display", "Monitor configuration",
                             "screen resolution") == TL_OK);
    CHECK(lexical_add_fields(engine, 2, "/Applications/Screen Recorder", "Capture", "display") ==
          TL_OK);
    CHECK(lexical_add(engine, 3, "/work/projectNotes.md", false) == TL_OK);
    CHECK(lexical_add(engine, 4, "/work/projectNotebook.md", false) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(lexical_add_fields(engine, 5, "/Applications/Late", NULL, NULL) == TL_STATE);
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    expect(engine, workspace, "screen", "/Applications/Screen Recorder");
    expect(engine, workspace, "monitor", "/Applications/Display");
    tl_result result[1];
    size_t count = 0;
    CHECK(lexical_query(engine, workspace, "monitor", result, 1, &count) == TL_OK);
    CHECK(count == 1 && result[0].score == 2800 + 128 + 57);
    expect(engine, workspace, "screen resolution", "/Applications/Display");
    expect(engine, workspace, "project notes", "/work/projectNotes.md");
    const char *queries[] = {"s",       "sc",          "screen",
                             "monitor", "resolution",  "screen resolution",
                             "proej",   "proej notes", "project notes"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++)
        capacity_consistency(engine, workspace, queries[i]);
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_add(engine, 1, "/work/projectNotes.md", false) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK && lexical_workspace_create(engine, &workspace) == TL_OK);
    expect(engine, workspace, "proej", "/work/projectNotes.md");
    lexical_workspace_destroy(workspace);
    lexical_destroy(engine);
}
void test_lexical(void) {
    first_token_completeness();
    quality_fields();
    prefix_bonus_ranking();
    evidence_eviction();
    parallel_ranking();
    multiword_bounds();
    complete_scan_regression();
    typo_and_trigram_channels();
    root_ancestors_are_not_context();
    warm_equals_fresh();
    parent_matches_stay_in_one_component();
    const char *paths[] = {"/work",
                           "/work/projectNotes.md",
                           "/work/z-project.md",
                           "/work/a-project.md",
                           "/project/other.txt",
                           "/work/Caf\xc3\xa9.pdf",
                           "/work/bad\xff.md",
                           "/work/bad\xfe.md",
                           "/work/my notes.txt"};
    tl_lexical *engine = NULL;
    tl_lexical_workspace *workspace = NULL, *cold = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_workspace_create(engine, &workspace) == TL_STATE);
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        CHECK(lexical_add(engine, i + 1, paths[i], i == 0) == TL_OK);
    CHECK(lexical_add(engine, 1, "bad", false) == TL_INVALID);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(lexical_add(engine, 100, "bad", false) == TL_STATE);
    const char *resolved = NULL;
    CHECK(lexical_resolve(engine, 7, &resolved) == TL_OK && strcmp(resolved, paths[6]) == 0);
    CHECK(lexical_workspace_create(engine, &workspace) == TL_OK);
    CHECK(lexical_workspace_create(engine, &cold) == TL_OK);
    expect(engine, workspace, "", paths[0]);
    expect(engine, workspace, "  ", paths[0]);
    expect(engine, workspace, "PROJECTNOTES.MD", paths[1]);
    expect(engine, workspace, "prjnts", paths[1]);
    expect(engine, workspace, "pn", paths[1]);
    expect(engine, workspace, "work prjnts", paths[1]);
    expect(engine, workspace, "CAFE\xcc\x81", paths[5]);
    expect(engine, workspace, "bad\xff", paths[6]);
    expect(engine, workspace, paths[8], paths[8]);
    const char *queries[] = {"p", "pr", "prj", "prjnts", "pr", "xyz", "project", "other project"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        tl_result warm[10], fresh[10];
        size_t nw = 0, nf = 0;
        CHECK(lexical_query(engine, workspace, queries[i], warm, 10, &nw) == TL_OK);
        CHECK(lexical_query(engine, cold, queries[i], fresh, 10, &nf) == TL_OK);
        CHECK(nw == nf);
        for (size_t j = 0; j < nw; j++)
            CHECK(warm[j].id == fresh[j].id && warm[j].score == fresh[j].score);
    }
    tl_result result[1];
    size_t count = 99;
    char large[LEXICAL_QUERY_BYTES + 2];
    memset(large, 'a', sizeof(large));
    large[sizeof(large) - 1] = 0;
    CHECK(lexical_query(engine, workspace, large, result, 1, &count) == TL_LIMIT && count == 0);
    CHECK(lexical_query(engine, workspace, "project", result, 0, &count) == TL_INVALID);
    expect(engine, workspace, "prjnts", paths[1]);
    lexical_workspace_destroy(workspace);
    lexical_workspace_destroy(cold);
    lexical_destroy(engine);
}
