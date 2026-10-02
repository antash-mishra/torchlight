/* Greedy subsequence scoring with boundary/consecutive bonuses, plus a
 * linear-time check for edit distance within one edit. */
#include "torchlight/fuzzy.h"
#include <limits.h>
#include <string.h>
enum {
    MATCH_BONUS = 16,
    BOUNDARY_BONUS = 32,
    CONSECUTIVE_BONUS = 24,
    MAX_GAP_PENALTY = 64,
    BASE_SCORE = 256,
    /* Largest possible gain per matched symbol, bounding the total score. */
    MAX_SYMBOL_GAIN = MATCH_BONUS + BOUNDARY_BONUS + CONSECUTIVE_BONUS
};
int fuzzy_score_bound(size_t query_length) {
    if (query_length > (size_t)(INT_MAX - BASE_SCORE) / MAX_SYMBOL_GAIN)
        return INT_MAX;
    return BASE_SCORE + (int)query_length * MAX_SYMBOL_GAIN;
}
tl_status fuzzy_score(tl_text text, tl_text query, int *out) {
    if (out == NULL || text.symbols == NULL || text.boundaries == NULL || query.symbols == NULL ||
        query.length == 0)
        return TL_INVALID;
    *out = 0;
    if (query.length > (size_t)(INT_MAX - BASE_SCORE) / MAX_SYMBOL_GAIN)
        return TL_LIMIT;
    if ((text.mask & query.mask) != query.mask)
        return TL_OK;
    size_t matched = 0, previous = 0;
    int score = BASE_SCORE;
    for (size_t i = 0; i < text.length && matched < query.length; i++) {
        if (text.symbols[i] != query.symbols[matched])
            continue;
        size_t gap = matched == 0 ? i : i - previous - 1;
        score += MATCH_BONUS;
        score += text.boundaries[i] != 0 ? BOUNDARY_BONUS : 0;
        score += matched != 0 && gap == 0 ? CONSECUTIVE_BONUS : 0;
        score -= gap > MAX_GAP_PENALTY ? MAX_GAP_PENALTY : (int)gap;
        previous = i;
        matched++;
    }
    if (matched == query.length)
        *out = score > 0 ? score : 1;
    return TL_OK;
}
static bool same_tail(const uint32_t *a, const uint32_t *b, size_t length) {
    return length == 0 || memcmp(a, b, length * sizeof(uint32_t)) == 0;
}
/* With a one-edit budget, everything before the first mismatch must be equal,
 * so only the single edit at that mismatch needs checking: no DP table. */
static size_t within_one(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) {
    const size_t over = FUZZY_EDIT_LIMIT + 1;
    if (na < nb)
        return within_one(b, nb, a, na);
    if (na - nb > 1)
        return over;
    size_t i = 0;
    while (i < nb && a[i] == b[i])
        i++;
    if (i == nb)
        return na - nb; /* identical, or a has one extra symbol at the end */
    if (na != nb)       /* deleting a[i] must leave the rest identical */
        return same_tail(a + i + 1, b + i, nb - i) ? 1 : over;
    if (same_tail(a + i + 1, b + i + 1, na - i - 1))
        return 1; /* substitution */
    bool swapped = i + 1 < na && a[i] == b[i + 1] && a[i + 1] == b[i];
    return swapped && same_tail(a + i + 2, b + i + 2, na - i - 2) ? 1 : over;
}
tl_status fuzzy_edit_distance(tl_text a, tl_text b, size_t *out) {
    if (out == NULL || (a.length != 0 && a.symbols == NULL) || (b.length != 0 && b.symbols == NULL))
        return TL_INVALID;
    *out = within_one(a.symbols, a.length, b.symbols, b.length);
    return TL_OK;
}
