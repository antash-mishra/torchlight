/* Relaxed overlap, frequent-trigram skipping, query limits and scratch reuse. */
#include "test.h"
#include "torchlight/trigram.h"
#include <string.h>
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
    CHECK(trigram_query(index, NULL, scratch, tokenize_view(q), collect, &hits) == TL_OK);
    tokenize_destroy(q);
    return hits;
}
/* Every reported slot and count must equal a brute-force overlap count with
 * the same frequent-trigram rule, whichever lists are scanned or probed. */
enum { BRUTE_SLOTS = 640, BRUTE_NAME = 9 };
struct brute_hits {
    size_t shared[BRUTE_SLOTS], count;
};
static tl_status collect_brute(void *context, size_t slot, size_t shared, size_t total) {
    struct brute_hits *hits = context;
    (void)total;
    CHECK(slot < BRUTE_SLOTS && hits->shared[slot] == 0);
    hits->shared[slot] = shared;
    hits->count++;
    return TL_OK;
}
static bool has_trigram(const char *name, const char *trigram) {
    for (size_t i = 0; i + 3 <= strlen(name); i++)
        if (memcmp(name + i, trigram, 3) == 0)
            return true;
    return false;
}
static void brute_force_overlap(void) {
    static char names[BRUTE_SLOTS][BRUTE_NAME];
    tl_trigram *index = NULL;
    tl_trigram_scratch *scratch = NULL;
    CHECK(trigram_create(&index) == TL_OK);
    uint64_t state = 11;
    for (size_t slot = 0; slot < BRUTE_SLOTS; slot++) {
        for (size_t i = 0; i + 1 < BRUTE_NAME; i++) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            names[slot][i] = "abcd"[(state >> 33) % 4]; /* few letters: frequent trigrams */
        }
        tl_tokenized *text = test_text(names[slot]);
        CHECK(trigram_add(index, tokenize_view(text), slot) == TL_OK);
        tokenize_destroy(text);
    }
    CHECK(trigram_finish(index) == TL_OK && trigram_scratch_create(index, &scratch) == TL_OK);
    const char *queries[] = {"abcdabc", "aabbccdd", "dcbadcba", "abababab", "zzabcz", "cdcdcdzz"};
    size_t limit = BRUTE_SLOTS / 8 > 64 ? BRUTE_SLOTS / 8 : 64;
    for (size_t q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
        static struct brute_hits hits;
        memset(&hits, 0, sizeof(hits));
        tl_tokenized *query = test_text(queries[q]);
        CHECK(trigram_query(index, NULL, scratch, tokenize_view(query), collect_brute, &hits) ==
              TL_OK);
        tokenize_destroy(query);
        /* Distinct query trigrams, informative unless in more than limit names. */
        char distinct[16][4];
        size_t keys = 0, informative = 0;
        bool useful[16] = {false};
        for (size_t i = 0; i + 3 <= strlen(queries[q]); i++) {
            bool seen = false;
            for (size_t k = 0; k < keys; k++)
                seen = seen || memcmp(distinct[k], queries[q] + i, 3) == 0;
            if (seen)
                continue;
            memcpy(distinct[keys], queries[q] + i, 3);
            distinct[keys][3] = 0;
            size_t postings = 0;
            for (size_t slot = 0; slot < BRUTE_SLOTS; slot++)
                postings += has_trigram(names[slot], distinct[keys]);
            useful[keys] = postings <= limit;
            informative += useful[keys];
            keys++;
        }
        /* Too few distinct trigrams: nothing is reported ("abababab"). */
        size_t needed = keys < TRIGRAM_MIN_QUERY_TRIGRAMS ? 0 : (informative + 1) / 2;
        size_t expected = 0;
        for (size_t slot = 0; slot < BRUTE_SLOTS; slot++) {
            size_t shared = 0;
            for (size_t k = 0; k < keys; k++)
                shared += useful[k] && has_trigram(names[slot], distinct[k]);
            bool reported = needed != 0 && shared >= needed;
            expected += reported;
            CHECK(hits.shared[slot] == (reported ? shared : 0));
        }
        CHECK(hits.count == expected);
    }
    trigram_scratch_destroy(scratch);
    trigram_destroy(index);
}
/* A delta index takes frequent-trigram decisions from its reference: a
 * trigram rare in the delta but frequent in the reference is uninformative. */
static void reference_decides_frequency(void) {
    tl_trigram *reference = NULL, *delta = NULL;
    tl_trigram_scratch *scratch = NULL;
    CHECK(trigram_create(&reference) == TL_OK && trigram_create(&delta) == TL_OK);
    for (size_t slot = 0; slot < 600; slot++) {
        tl_tokenized *text = test_text(slot % 2 == 0 ? "commonabc" : "zzzzzzzzz");
        CHECK(trigram_add(reference, tokenize_view(text), slot) == TL_OK);
        tokenize_destroy(text);
    }
    tl_tokenized *name = test_text("commxyzq");
    CHECK(trigram_add(delta, tokenize_view(name), 0) == TL_OK);
    tokenize_destroy(name);
    CHECK(trigram_finish(reference) == TL_OK && trigram_finish(delta) == TL_OK);
    CHECK(trigram_scratch_create(delta, &scratch) == TL_OK);
    /* "commxy": com, omm, mmx, mxy. Alone, the delta shares all four; with the
     * reference, com and omm are frequent, leaving mmx and mxy (2 of 2). */
    tl_tokenized *query = test_text("commxy");
    struct hits hits = {0};
    CHECK(trigram_query(delta, NULL, scratch, tokenize_view(query), collect, &hits) == TL_OK);
    CHECK(hits.count == 1 && hits.shared[0] == 4 && hits.total[0] == 4);
    hits = (struct hits){0};
    CHECK(trigram_query(delta, reference, scratch, tokenize_view(query), collect, &hits) == TL_OK);
    CHECK(hits.count == 1 && hits.shared[0] == 2 && hits.total[0] == 2);
    tokenize_destroy(query);
    trigram_scratch_destroy(scratch);
    trigram_destroy(delta);
    trigram_destroy(reference);
}
void test_trigram(void) {
    brute_force_overlap();
    reference_decides_frequency();
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
    CHECK(trigram_query(index, NULL, scratch, tokenize_view(q), NULL, NULL) == TL_INVALID);
    tokenize_destroy(q);
    trigram_scratch_destroy(scratch);
    trigram_destroy(index);
    for (size_t i = 0; i < 4; i++)
        tokenize_destroy(texts[i]);
}
