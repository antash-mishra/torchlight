/* Complete scans, deterministic top-k, query edits and byte-safe exact paths. */
#include "test.h"
#include "torchlight/lexical.h"
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
    CHECK(lexical_finish(engine) == TL_OK);
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
void test_lexical(void) {
    complete_scan_regression();
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
