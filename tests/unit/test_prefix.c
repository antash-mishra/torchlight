/* Sorted ranges, token initials, short queries and basename priority. */
#include "test.h"
#include "torchlight/prefix.h"
struct best_scores {
    int scores[2];
    size_t hits;
};
static tl_status keep_best(void *context, size_t slot, int score) {
    struct best_scores *best = context;
    CHECK(slot < 2);
    if (score > best->scores[slot])
        best->scores[slot] = score;
    best->hits++;
    return TL_OK;
}
static tl_status stop(void *context, size_t slot, int score) {
    (void)context;
    (void)slot;
    (void)score;
    return TL_LIMIT;
}
static struct best_scores query(const tl_prefix *index, const char *text) {
    struct best_scores best = {{0, 0}, 0};
    tl_tokenized *q = test_text(text);
    CHECK(prefix_query(index, tokenize_view(q), keep_best, &best) == TL_OK);
    tokenize_destroy(q);
    return best;
}
void test_prefix(void) {
    tl_prefix *index = NULL;
    CHECK(prefix_create(&index) == TL_OK);
    tl_tokenized *a = test_text("/projectNotes/other.txt"), *b = test_text("/work/projectNotes.md");
    tl_tokenized *q = test_text("pr");
    struct best_scores best = {{0, 0}, 0};
    CHECK(prefix_query(index, tokenize_view(q), keep_best, &best) == TL_STATE);
    CHECK(prefix_add(index, tokenize_view(a), 0) == TL_OK);
    CHECK(prefix_add(index, tokenize_view(b), 1) == TL_OK);
    CHECK(prefix_finish(index) == TL_OK);
    CHECK(prefix_finish(index) == TL_STATE);
    CHECK(prefix_query(index, tokenize_view(q), NULL, &best) == TL_INVALID);
    CHECK(prefix_query(index, tokenize_view(q), stop, NULL) == TL_LIMIT);
    best = query(index, "pr");
    CHECK(best.scores[1] == PREFIX_BASENAME_SCORE && best.scores[0] == PREFIX_PARENT_SCORE);
    best = query(index, "project");
    CHECK(best.scores[1] == PREFIX_BASENAME_SCORE + PREFIX_COMPLETE_BONUS &&
          best.scores[0] == PREFIX_PARENT_SCORE);
    best = query(index, "projectn");
    CHECK(best.scores[1] == PREFIX_BASENAME_SCORE && best.scores[0] == 0);
    best = query(index, "projectnotes.md");
    CHECK(best.scores[1] == PREFIX_BASENAME_SCORE && best.scores[0] == 0);
    best = query(index, "pn");
    CHECK(best.scores[1] == PREFIX_INITIALS_SCORE && best.scores[0] == 0);
    best = query(index, "notes");
    CHECK(best.scores[1] == PREFIX_TOKEN_SCORE + PREFIX_COMPLETE_BONUS &&
          best.scores[0] == PREFIX_PARENT_SCORE);
    best = query(index, "zzz");
    CHECK(best.hits == 0);
    prefix_destroy(index);
    tokenize_destroy(a);
    tokenize_destroy(b);
    tokenize_destroy(q);
}
