/* Sorted ranges, token initials, short queries and basename priority. */
#include "test.h"
#include "torchlight/prefix.h"
void test_prefix(void) {
    tl_prefix *index = NULL;
    CHECK(prefix_create(&index) == TL_OK);
    tl_tokenized *a = test_text("/projectNotes/other.txt"), *b = test_text("/work/projectNotes.md");
    tl_tokenized *q = test_text("pr");
    int scores[2] = {0};
    CHECK(prefix_query(index, tokenize_view(q), scores, 2) == TL_STATE);
    CHECK(prefix_add(index, tokenize_view(a), 0) == TL_OK);
    CHECK(prefix_add(index, tokenize_view(b), 1) == TL_OK);
    CHECK(prefix_finish(index) == TL_OK);
    CHECK(prefix_finish(index) == TL_STATE);
    CHECK(prefix_query(index, tokenize_view(q), scores, 1) == TL_LIMIT);
    CHECK(prefix_query(index, tokenize_view(q), scores, 2) == TL_OK);
    CHECK(scores[1] > scores[0] && scores[0] > 0);
    tokenize_destroy(q);
    q = test_text("pn");
    scores[0] = 0;
    scores[1] = 0;
    CHECK(prefix_query(index, tokenize_view(q), scores, 2) == TL_OK && scores[1] > 0);
    prefix_destroy(index);
    tokenize_destroy(a);
    tokenize_destroy(b);
    tokenize_destroy(q);
}
