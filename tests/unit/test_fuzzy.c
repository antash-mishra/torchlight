/* Boundary/consecutive bonuses, score bounds and one-edit distance checks. */
#include "test.h"
#include "torchlight/fuzzy.h"
#include <string.h>
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
/* Exhaustive alignment oracle, independent of the DP running maxima. */
static int alignment(tl_text text, tl_text query, size_t matched, size_t start, size_t previous,
                     int score) {
    if (matched == query.length)
        return score > 0 ? score : 1;
    int best = 0;
    for (size_t i = start; i < text.length; i++) {
        if (text.symbols[i] != query.symbols[matched])
            continue;
        size_t gap = matched == 0 ? i : i - previous - 1;
        int next = score + 16 + (text.boundaries[i] != 0 ? 32 : 0) +
                   (matched != 0 && gap == 0 ? 24 : 0) - (int)(gap > 64 ? 64 : gap);
        int value = alignment(text, query, matched + 1, i + 1, i, next);
        if (value > best)
            best = value;
    }
    return best;
}
static void optimal_alignments(void) {
    uint32_t symbols[7], pattern[4];
    uint8_t boundaries[7];
    for (unsigned t = 0; t < 128; t++) {
        for (size_t i = 0; i < 7; i++) {
            symbols[i] = 'a' + ((t >> i) & 1U);
            boundaries[i] = (uint8_t)(i % 3 == 0);
        }
        tl_text text = {.symbols = symbols, .boundaries = boundaries, .length = 7};
        for (unsigned q = 0; q < 16; q++) {
            for (size_t i = 0; i < 4; i++)
                pattern[i] = 'a' + ((q >> i) & 1U);
            tl_text query = {.symbols = pattern, .length = 4};
            int optimal = 0, greedy = 0;
            CHECK(fuzzy_score(text, query, &optimal) == TL_OK);
            CHECK(optimal == alignment(text, query, 0, 0, 0, 256));
            CHECK(fuzzy_score_greedy(text, query, &greedy) == TL_OK);
            CHECK(optimal >= greedy && optimal <= fuzzy_score_bound(4));
        }
    }
    tl_tokenized *text = test_text("axxxxxxxxxx_abc"), *query = test_text("abc");
    int optimal = 0, greedy = 0;
    CHECK(fuzzy_score(tokenize_view(text), tokenize_view(query), &optimal) == TL_OK);
    CHECK(fuzzy_score_greedy(tokenize_view(text), tokenize_view(query), &greedy) == TL_OK);
    CHECK(optimal > greedy);
    tokenize_destroy(text);
    tokenize_destroy(query);
    char long_name[FUZZY_OPTIMAL_MAX_SYMBOLS + 2];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = 0;
    text = test_text(long_name);
    query = test_text("aaa");
    CHECK(fuzzy_score(tokenize_view(text), tokenize_view(query), &optimal) == TL_OK);
    CHECK(fuzzy_score_greedy(tokenize_view(text), tokenize_view(query), &greedy) == TL_OK);
    CHECK(optimal == greedy);
    tokenize_destroy(text);
    tokenize_destroy(query);
    char gap_name[72];
    memset(gap_name, 'x', sizeof(gap_name) - 1);
    gap_name[0] = 'a';
    gap_name[70] = 'b';
    gap_name[71] = 0;
    text = test_text(gap_name);
    query = test_text("ab");
    CHECK(fuzzy_score(tokenize_view(text), tokenize_view(query), &optimal) == TL_OK);
    CHECK(optimal == alignment(tokenize_view(text), tokenize_view(query), 0, 0, 0, 256));
    tokenize_destroy(text);
    tokenize_destroy(query);
}
void test_fuzzy(void) {
    optimal_alignments();
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
