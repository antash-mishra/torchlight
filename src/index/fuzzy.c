/* Bounded boundary/consecutive bonuses for a greedy subsequence baseline. */
#include "torchlight/fuzzy.h"
#include <limits.h>
tl_status fuzzy_score(tl_text text, tl_text query, int *out) {
    enum {
        MATCH_BONUS = 16,
        BOUNDARY_BONUS = 32,
        CONSECUTIVE_BONUS = 24,
        MAX_GAP_PENALTY = 64,
        BASE_SCORE = 256,
        /* Largest possible gain per matched symbol, bounding the total score. */
        MAX_SYMBOL_GAIN = MATCH_BONUS + BOUNDARY_BONUS + CONSECUTIVE_BONUS
    };
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
