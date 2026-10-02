/* Relaxed overlap, frequent-trigram skipping, query limits and scratch reuse. */
#include "test.h"
#include "torchlight/trigram.h"
struct hits {
    size_t count, last_slot, shared[8], total[8];
};
static tl_status collect(void *context, size_t slot, size_t shared, size_t total) {
    struct hits *hits = context;
    CHECK(slot < 8);
    hits->shared[slot] = shared;
    hits->total[slot] = total;
    hits->last_slot = slot;
    hits->count++;
    return TL_OK;
}
static struct hits query(const tl_trigram *index, tl_trigram_scratch *scratch, const char *text) {
    struct hits hits = {0};
    tl_tokenized *q = test_text(text);
    CHECK(trigram_query(index, scratch, tokenize_view(q), collect, &hits) == TL_OK);
    tokenize_destroy(q);
    return hits;
}
void test_trigram(void) {
    const char *names[] = {"projectnotes.md", "project.txt", "notes.txt", "zzz"};
    tl_tokenized *texts[4];
    tl_trigram *index = NULL;
    tl_trigram_scratch *scratch = NULL;
    CHECK(trigram_create(&index) == TL_OK);
    for (size_t i = 0; i < 4; i++) {
        texts[i] = test_text(names[i]);
        CHECK(trigram_add(index, tokenize_view(texts[i]), i * 2) == TL_OK);
    }
    CHECK(trigram_add(index, tokenize_view(texts[0]), 6) == TL_INVALID); /* not increasing */
    CHECK(trigram_scratch_create(index, &scratch) == TL_STATE);
    CHECK(trigram_finish(index) == TL_OK && trigram_finish(index) == TL_STATE);
    CHECK(trigram_scratch_create(index, &scratch) == TL_OK);
    /* "projc": pro, roj, ojc; two of three are shared with both project names. */
    struct hits hits = query(index, scratch, "projc");
    CHECK(hits.count == 2 && hits.shared[0] == 2 && hits.total[0] == 3 && hits.shared[2] == 2);
    /* A transposed typo keeps enough trigrams of the long name. */
    hits = query(index, scratch, "projectntoes");
    CHECK(hits.count >= 1 && hits.shared[0] * 2 >= hits.total[0]);
    /* Too short: fewer than TRIGRAM_MIN_QUERY_TRIGRAMS distinct trigrams. */
    CHECK(query(index, scratch, "proj").count == 0);
    CHECK(query(index, scratch, "qqqqqqq").count == 0);
    /* Scratch is reusable: the same query reports the same counts again. */
    hits = query(index, scratch, "projc");
    CHECK(hits.count == 2 && hits.shared[0] == 2);
    tl_tokenized *q = test_text("projc");
    CHECK(trigram_query(index, scratch, tokenize_view(q), NULL, NULL) == TL_INVALID);
    tokenize_destroy(q);
    trigram_scratch_destroy(scratch);
    trigram_destroy(index);
    for (size_t i = 0; i < 4; i++)
        tokenize_destroy(texts[i]);
}
