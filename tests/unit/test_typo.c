/* One-edit lookup of complete tokens: edits, exact-token exclusion, digits. */
#include "test.h"
#include "torchlight/typo.h"
struct hits {
    size_t count;
    bool slots[4];
};
static tl_status collect(void *context, size_t slot) {
    struct hits *hits = context;
    CHECK(slot < 4);
    hits->slots[slot] = true;
    hits->count++;
    return TL_OK;
}
static struct hits query(const tl_typo *index, tl_typo_scratch *scratch, const char *text) {
    struct hits hits = {0};
    tl_tokenized *q = test_text(text);
    CHECK(typo_query(index, scratch, tokenize_view(q), collect, &hits) == TL_OK);
    tokenize_destroy(q);
    return hits;
}
static void length_boundaries(void) {
    tl_typo *index = NULL;
    tl_typo_scratch *scratch = NULL;
    tl_tokenized *short_name = test_text("mad.txt");
    tl_tokenized *long_name = test_text("abcdefghijklmnopqrstuvwxyzabcdef.txt");
    CHECK(typo_create(&index) == TL_OK);
    CHECK(typo_add(index, tokenize_view(short_name), 0) == TL_OK);
    CHECK(typo_add(index, tokenize_view(long_name), 1) == TL_OK);
    CHECK(typo_finish(index) == TL_OK && typo_scratch_create(index, &scratch) == TL_OK);
    /* Both are one edit from indexed tokens, but outside the query budget. */
    CHECK(query(index, scratch, "md").count == 0);
    CHECK(query(index, scratch, "abcdefghijklmnopqrstuvwxyzzabcdef").count == 0);
    CHECK(query(index, scratch, "mXd").slots[0]);
    CHECK(query(index, scratch, "abcdefghijklmnopqrstuvwxyzabcde").slots[1]);
    typo_scratch_destroy(scratch);
    typo_destroy(index);
    tokenize_destroy(short_name);
    tokenize_destroy(long_name);
}
void test_typo(void) {
    length_boundaries();
    const char *names[] = {"README.md", "projectNotes.md", "IMG_20240101.jpg", "readme-old.txt"};
    tl_tokenized *texts[4];
    tl_typo *index = NULL;
    tl_typo_scratch *scratch = NULL;
    CHECK(typo_create(&index) == TL_OK);
    for (size_t i = 0; i < 4; i++) {
        texts[i] = test_text(names[i]);
        CHECK(typo_add(index, tokenize_view(texts[i]), i) == TL_OK);
    }
    CHECK(typo_finish(index) == TL_OK && typo_finish(index) == TL_STATE);
    CHECK(typo_scratch_create(index, &scratch) == TL_OK);
    /* Swap, deletion, insertion and substitution all reach "readme" (both names). */
    const char *edits[] = {"raedme", "redme", "readmee", "reXdme"};
    for (size_t i = 0; i < 4; i++) {
        struct hits hits = query(index, scratch, edits[i]);
        CHECK(hits.slots[0] && hits.slots[3] && !hits.slots[1] && !hits.slots[2]);
    }
    /* Exact tokens are the prefix channel's job; camelCase tokens are indexed. */
    CHECK(query(index, scratch, "readme").count == 0);
    CHECK(query(index, scratch, "ntoes").slots[1]);
    /* All-digit tokens are not indexed, so a digit typo finds nothing. */
    CHECK(query(index, scratch, "20240102").count == 0);
    /* Two edits away, or too short, find nothing. */
    CHECK(query(index, scratch, "rdeame").count == 0 && query(index, scratch, "md").count == 0);
    typo_scratch_destroy(scratch);
    typo_destroy(index);
    for (size_t i = 0; i < 4; i++)
        tokenize_destroy(texts[i]);
}
