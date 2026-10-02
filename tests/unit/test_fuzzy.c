/* Boundary/consecutive bonuses, score bounds and one-edit distance checks. */
#include "test.h"
#include "torchlight/fuzzy.h"
static size_t distance(const char *a, const char *b) {
    tl_tokenized *x = test_text(a), *y = test_text(b);
    size_t out = 99;
    CHECK(fuzzy_edit_distance(tokenize_view(x), tokenize_view(y), &out) == TL_OK);
    tokenize_destroy(x);
    tokenize_destroy(y);
    return out;
}
static void edit_distances(void) {
    CHECK(distance("readme", "readme") == 0);
    CHECK(distance("readme", "raedme") == 1);  /* adjacent swap */
    CHECK(distance("readme", "redme") == 1);   /* deletion */
    CHECK(distance("readme", "readmes") == 1); /* insertion at the end */
    CHECK(distance("readme", "xreadme") == 1); /* insertion at the start */
    CHECK(distance("readme", "reXdme") == 1);  /* substitution */
    CHECK(distance("readme", "rdeame") == 2);
    CHECK(distance("readme", "read") == 2);
    CHECK(distance("ab", "ba") == 1 && distance("abc", "cab") == 2);
    CHECK(distance("", "a") == 1 && distance("", "ab") == 2);
    size_t out = 0;
    tl_text broken = {.symbols = NULL, .length = 2};
    CHECK(fuzzy_edit_distance(broken, broken, &out) == TL_INVALID);
    CHECK(fuzzy_edit_distance((tl_text){0}, (tl_text){0}, NULL) == TL_INVALID);
}
void test_fuzzy(void) {
    tl_tokenized *a = test_text("projectNotes"), *b = test_text("pzzzzn"), *q = test_text("pn");
    int good = 0, bad = 0;
    CHECK(fuzzy_score(tokenize_view(a), tokenize_view(q), &good) == TL_OK);
    CHECK(fuzzy_score(tokenize_view(b), tokenize_view(q), &bad) == TL_OK);
    CHECK(good > bad && bad > 0);
    CHECK(good <= fuzzy_score_bound(2) && fuzzy_score_bound(3) > fuzzy_score_bound(2));
    tokenize_destroy(q);
    q = test_text("projectnotes");
    CHECK(fuzzy_score(tokenize_view(a), tokenize_view(q), &good) == TL_OK);
    CHECK(good > 0 && good <= fuzzy_score_bound(12));
    tokenize_destroy(q);
    q = test_text("xyz");
    CHECK(fuzzy_score(tokenize_view(a), tokenize_view(q), &good) == TL_OK && good == 0);
    CHECK(fuzzy_score(tokenize_view(a), tokenize_view(q), NULL) == TL_INVALID);
    tokenize_destroy(a);
    tokenize_destroy(b);
    tokenize_destroy(q);
    edit_distances();
}
