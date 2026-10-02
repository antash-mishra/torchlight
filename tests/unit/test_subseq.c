/* Abbreviations, opaque bytes, conservative masks and the membership cache. */
#include "test.h"
#include "torchlight/subseq.h"
static void membership_cache(void) {
    tl_subseq_cache *cache = NULL;
    CHECK(subseq_cache_create(4, &cache) == TL_OK);
    tl_tokenized *pr = test_text("pr"), *prj = test_text("prj"), *p = test_text("p");
    const uint32_t *members = NULL;
    size_t count = 0;
    CHECK(!subseq_cache_lookup(cache, tokenize_view(pr), 0, &members, &count));
    CHECK(subseq_cache_commit(cache, 0) == TL_STATE);
    uint32_t *record = NULL;
    CHECK(subseq_cache_begin(cache, tokenize_view(pr), 0, &record) == TL_OK);
    record[0] = 3;
    record[1] = 1;
    CHECK(subseq_cache_commit(cache, 2) == TL_OK);
    /* Extensions with the same mode reuse it; shorter or other modes do not. */
    CHECK(subseq_cache_lookup(cache, tokenize_view(prj), 0, &members, &count));
    CHECK(count == 2 && members[0] == 3 && members[1] == 1);
    CHECK(subseq_cache_lookup(cache, tokenize_view(pr), 0, &members, &count));
    CHECK(!subseq_cache_lookup(cache, tokenize_view(p), 0, &members, &count));
    CHECK(!subseq_cache_lookup(cache, tokenize_view(prj), 1, &members, &count));
    /* Recording never overwrites the membership being read. */
    CHECK(subseq_cache_begin(cache, tokenize_view(prj), 0, &record) == TL_OK);
    CHECK(record != members);
    record[0] = 3;
    CHECK(subseq_cache_commit(cache, 5) == TL_LIMIT && subseq_cache_commit(cache, 1) == TL_OK);
    CHECK(subseq_cache_lookup(cache, tokenize_view(prj), 0, &members, &count) && count == 1);
    tokenize_destroy(pr);
    tokenize_destroy(prj);
    tokenize_destroy(p);
    subseq_cache_destroy(cache);
}
void test_subseq(void) {
    membership_cache();
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
