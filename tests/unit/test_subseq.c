/* Abbreviations, opaque bytes and conservative-mask collisions. */
#include "test.h"
#include "torchlight/subseq.h"
void test_subseq(void) {
    tl_tokenized *text = test_text("projectNotes.md"), *query = test_text("prjnts");
    CHECK(subseq_matches(tokenize_view(text), tokenize_view(query)));
    tokenize_destroy(query);
    query = test_text("ntsprj");
    CHECK(!subseq_matches(tokenize_view(text), tokenize_view(query)));
    tokenize_destroy(text);
    tokenize_destroy(query);
    text = test_text("/a\xff"
                     "b");
    query = test_text("\xff");
    CHECK(subseq_matches(tokenize_view(text), tokenize_view(query)));
    tokenize_destroy(query);
    query = test_text("\xfe");
    CHECK(!subseq_matches(tokenize_view(text), tokenize_view(query)));
    tokenize_destroy(text);
    tokenize_destroy(query);
    CHECK(subseq_matches((tl_text){0}, (tl_text){0}));
    uint32_t a = 'a', b = 'b';
    tl_text x = {.symbols = &a, .length = 1, .mask = UINT64_MAX};
    tl_text y = {.symbols = &b, .length = 1, .mask = UINT64_MAX};
    CHECK(!subseq_matches(x, y));
}
