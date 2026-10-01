/* Boundary/consecutive bonuses versus scattered matches. */
#include "test.h"
#include "torchlight/fuzzy.h"
void test_fuzzy(void) {
    tl_tokenized *a = test_text("projectNotes"), *b = test_text("pzzzzn"), *q = test_text("pn");
    int good = 0, bad = 0;
    CHECK(fuzzy_score(tokenize_view(a), tokenize_view(q), &good) == TL_OK);
    CHECK(fuzzy_score(tokenize_view(b), tokenize_view(q), &bad) == TL_OK);
    CHECK(good > bad && bad > 0);
    tokenize_destroy(q);
    q = test_text("xyz");
    CHECK(fuzzy_score(tokenize_view(a), tokenize_view(q), &good) == TL_OK && good == 0);
    CHECK(fuzzy_score(tokenize_view(a), tokenize_view(q), NULL) == TL_INVALID);
    tokenize_destroy(a);
    tokenize_destroy(b);
    tokenize_destroy(q);
}
