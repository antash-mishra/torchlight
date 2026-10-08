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
/* Best score and completeness per slot, as prefix_query reports them. */
struct indexed_scores {
    int best[8];
    bool complete[8];
};
static tl_status keep_indexed(void *context, size_t slot, int score) {
    struct indexed_scores *scores = context;
    CHECK(slot < 8);
    if (score > scores->best[slot])
        scores->best[slot] = score;
    if (score == PREFIX_BASENAME_SCORE + PREFIX_COMPLETE_BONUS ||
        score == PREFIX_TOKEN_SCORE + PREFIX_COMPLETE_BONUS)
        scores->complete[slot] = true;
    return TL_OK;
}
/* prefix_score must report exactly what prefix_query reports per slot. */
static void score_matches_query(void) {
    const char *texts[] = {"/projectNotes/other.txt",
                           "/work/projectNotes.md",
                           "/a/p",
                           "/x/HTMLParser v2.txt",
                           "/r/s s.md",
                           "/q/Caf\xc3\xa9 au lait"};
    const char *queries[] = {"p", "pr", "pn", "o",   "h",           "hp", "parser", "v2",
                             "s", "s.", "c",  "cal", "caf\xc3\xa9", "z",  "md",     "projectnotes"};
    size_t count = sizeof(texts) / sizeof(texts[0]);
    tl_tokenized *tokens[8];
    tl_prefix *index = NULL;
    CHECK(prefix_create(&index) == TL_OK);
    for (size_t i = 0; i < count; i++) {
        tokens[i] = test_text(texts[i]);
        CHECK(prefix_add(index, tokenize_view(tokens[i]), i) == TL_OK);
    }
    CHECK(prefix_finish(index) == TL_OK);
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        tl_tokenized *query = test_text(queries[q]);
        struct indexed_scores expected = {{0}, {false}};
        CHECK(prefix_query(index, tokenize_view(query), keep_indexed, &expected) == TL_OK);
        for (size_t i = 0; i < count; i++) {
            int best = -1;
            bool complete = true;
            CHECK(prefix_score(tokenize_view(tokens[i]), tokenize_view(query), &best, &complete) ==
                  TL_OK);
            CHECK(best == expected.best[i] && complete == expected.complete[i]);
        }
        tokenize_destroy(query);
    }
    int best = 1;
    bool complete = true;
    tl_text empty = {0};
    CHECK(prefix_score(tokenize_view(tokens[0]), empty, &best, &complete) == TL_INVALID &&
          best == 0 && !complete);
    for (size_t i = 0; i < count; i++)
        tokenize_destroy(tokens[i]);
    prefix_destroy(index);
}
void test_prefix(void) {
    score_matches_query();
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
