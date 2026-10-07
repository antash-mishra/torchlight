/* Optimal bounded subsequence scoring with a greedy fallback, a Frizbee SIMD
 * matcher weighted to produce the same scale, and a linear-time check for edit
 * distance within one edit. */
#include "torchlight/fuzzy.h"
#include <frizbee.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>
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
tl_status fuzzy_score_greedy(tl_text text, tl_text query, int *out) {
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
/* The two running maxima encode capped gaps exactly: max(previous[j] + j)
 * handles the linear penalty and max(previous[j]) handles its capped tail.
 * Thus each DP row is linear, without a quadratic search over alignments. */
static void alignment_row(tl_text text, uint32_t symbol, const int *previous, int *current) {
    const int missing = INT_MIN / 2;
    int best = missing, positioned = missing;
    for (size_t i = 0; i < text.length; i++) {
        if (i >= 2 && previous[i - 2] != missing) {
            if (previous[i - 2] > best)
                best = previous[i - 2];
            if (previous[i - 2] + (int)i - 2 > positioned)
                positioned = previous[i - 2] + (int)i - 2;
        }
        int value = missing;
        if (i != 0 && previous[i - 1] != missing)
            value = previous[i - 1] + CONSECUTIVE_BONUS;
        int gap = positioned - (int)i + 1;
        if (best - MAX_GAP_PENALTY > gap)
            gap = best - MAX_GAP_PENALTY;
        if (best != missing && gap > value)
            value = gap;
        current[i] = text.symbols[i] == symbol && value != missing
                         ? value + MATCH_BONUS + (text.boundaries[i] != 0 ? BOUNDARY_BONUS : 0)
                         : missing;
    }
}
/* Masks cannot prove ordered membership. Reject impossible alignments in one
 * pass before paying for DP, especially for long abbreviations in noisy names. */
static bool ordered_match(tl_text text, tl_text query) {
    size_t matched = 0;
    for (size_t i = 0; i < text.length && matched < query.length; i++)
        if (text.symbols[i] == query.symbols[matched])
            matched++;
    return matched == query.length;
}
tl_status fuzzy_score(tl_text text, tl_text query, int *out) {
    if (text.length > FUZZY_OPTIMAL_MAX_SYMBOLS)
        return fuzzy_score_greedy(text, query, out);
    if (out == NULL || text.symbols == NULL || text.boundaries == NULL || query.symbols == NULL ||
        query.length == 0)
        return TL_INVALID;
    *out = 0;
    if (query.length > (size_t)(INT_MAX - BASE_SCORE) / MAX_SYMBOL_GAIN)
        return TL_LIMIT;
    if (query.length > text.length || (text.mask & query.mask) != query.mask ||
        !ordered_match(text, query))
        return TL_OK;
    int previous[FUZZY_OPTIMAL_MAX_SYMBOLS], current[FUZZY_OPTIMAL_MAX_SYMBOLS];
    const int missing = INT_MIN / 2;
    for (size_t i = 0; i < text.length; i++) {
        int gain = MATCH_BONUS + (text.boundaries[i] != 0 ? BOUNDARY_BONUS : 0);
        previous[i] = text.symbols[i] == query.symbols[0]
                          ? BASE_SCORE + gain - (int)(i > MAX_GAP_PENALTY ? MAX_GAP_PENALTY : i)
                          : missing;
    }
    for (size_t q = 1; q < query.length; q++) {
        alignment_row(text, query.symbols[q], previous, current);
        memcpy(previous, current, text.length * sizeof(*previous));
    }
    int best = missing;
    for (size_t i = 0; i < text.length; i++)
        if (previous[i] > best)
            best = previous[i];
    if (best != missing)
        *out = best > 0 ? best : 1;
    return TL_OK;
}
/* ---- Frizbee matcher ---------------------------------------------------- */
/* Frizbee's affine Smith-Waterman weights mirror the portable scorer, so one
 * scale and one bound serve both:
 *   - a matched symbol earns MATCH + CONSECUTIVE; a gap of g symbols then costs
 *     the lost consecutive bonus plus g, i.e. gap_open CONSECUTIVE + 1 and
 *     gap_extend 1, exactly the portable penalty for g <= MAX_GAP_PENALTY;
 *   - word starts earn BOUNDARY: after a delimiter, at a lower->upper change
 *     and (as Frizbee's prefix bonus) at the first symbol;
 *   - a substitution never pays, and exactness/case belong to the orchestrator.
 * The first symbol carries no consecutive bonus in the portable scorer, so
 * MATCHER_OFFSET = BASE - CONSECUTIVE makes a consecutive run from the start
 * score identically. Frizbee's maximum for n symbols is n * (MATCH +
 * CONSECUTIVE + BOUNDARY), so mapped scores stay within fuzzy_score_bound(n).
 * Differences that remain: no leading-gap penalty, and no bonus at
 * letter/digit changes. */
enum {
    MATCHER_MATCH = MATCH_BONUS + CONSECUTIVE_BONUS,
    MATCHER_GAP_OPEN = CONSECUTIVE_BONUS + 1,
    MATCHER_GAP_EXTEND = 1,
    MATCHER_MISMATCH = MAX_GAP_PENALTY,
    MATCHER_OFFSET = BASE_SCORE - CONSECUTIVE_BONUS,
    ASCII_LIMIT = 0x80
};
struct tl_fuzzy_matcher {
    frizbee_matcher_t *frizbee; /* NULL: the word keeps the portable scorer */
    tl_text word;               /* views symbols/boundaries below */
    int bound;
    char scratch[FUZZY_MATCHER_MAX_BYTES];
    /* word symbols, then boundaries, follow the struct (one allocation) */
};
static frizbee_config_t matcher_config(void) {
    frizbee_config_t config = frizbee_config_default();
    config.max_typos = 0; /* ordered subsequence membership, as fuzzy_score */
    config.casing = FRIZBEE_CASE_IGNORE;
    config.unicode = FRIZBEE_UNICODE_SMART;
    config.matching = FRIZBEE_MATCHING_FUZZY;
    config.scoring = (frizbee_scoring_t){.match_score = MATCHER_MATCH,
                                         .mismatch_penalty = MATCHER_MISMATCH,
                                         .gap_open_penalty = MATCHER_GAP_OPEN,
                                         .gap_extend_penalty = MATCHER_GAP_EXTEND,
                                         .prefix_bonus = BOUNDARY_BONUS,
                                         .capitalization_bonus = BOUNDARY_BONUS,
                                         .matching_case_bonus = 0,
                                         .exact_match_bonus = 0,
                                         .delimiter_bonus = BOUNDARY_BONUS};
    return config;
}
static bool lower_ascii(char c) {
    return c >= 'a' && c <= 'z';
}
/* Encode normalized symbols as UTF-8. A lowercase ASCII letter at a tokenizer
 * boundary right after another lowercase letter is written in upper case, so
 * Frizbee's capitalization bonus fires at camelCase and acronym boundaries of
 * the case-folded text. False for opaque symbols or when out is too small. */
static bool encode_text(tl_text text, char *out, size_t capacity, size_t *length, bool *ascii) {
    size_t used = 0;
    *ascii = true;
    for (size_t i = 0; i < text.length; i++) {
        uint32_t symbol = text.symbols[i];
        if (symbol < ASCII_LIMIT && used < capacity) {
            char c = (char)symbol;
            bool camel = text.boundaries[i] != 0 && used != 0 && lower_ascii(out[used - 1]);
            out[used++] = camel && lower_ascii(c) ? (char)(c - 'a' + 'A') : c;
            continue;
        }
        /* utf8proc_encode_char writes at most four bytes. */
        if (symbol >= TOKENIZE_OPAQUE_BASE || symbol < ASCII_LIMIT || capacity - used < 4)
            return false;
        *ascii = false;
        used +=
            (size_t)utf8proc_encode_char((utf8proc_int32_t)symbol, (utf8proc_uint8_t *)out + used);
    }
    *length = used;
    return true;
}
tl_status fuzzy_matcher_create(tl_text word, tl_fuzzy_matcher **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (word.symbols == NULL || word.boundaries == NULL || word.length == 0)
        return TL_INVALID;
    size_t extra = 0;
    if (tl_size_multiply(word.length, sizeof(uint32_t) + sizeof(uint8_t), &extra) != TL_OK ||
        extra > SIZE_MAX - sizeof(tl_fuzzy_matcher))
        return TL_LIMIT;
    tl_fuzzy_matcher *matcher = malloc(sizeof(*matcher) + extra);
    if (matcher == NULL)
        return TL_NOMEM;
    uint32_t *symbols = (uint32_t *)(matcher + 1);
    uint8_t *boundaries = (uint8_t *)(symbols + word.length);
    memcpy(symbols, word.symbols, word.length * sizeof(uint32_t));
    memcpy(boundaries, word.boundaries, word.length);
    matcher->word = word;
    matcher->word.symbols = symbols;
    matcher->word.boundaries = boundaries;
    matcher->word.byte_offsets = NULL;
    matcher->bound = fuzzy_score_bound(word.length);
    matcher->frizbee = NULL;
    size_t length = 0;
    bool ascii = true;
    if (word.length <= FUZZY_MATCHER_MAX_SYMBOLS &&
        encode_text(word, matcher->scratch, sizeof(matcher->scratch), &length, &ascii)) {
        frizbee_config_t config = matcher_config();
        matcher->frizbee =
            frizbee_matcher_new((frizbee_str_t){.ptr = matcher->scratch, .len = length}, &config);
    }
    *out = matcher;
    return TL_OK;
}
void fuzzy_matcher_destroy(tl_fuzzy_matcher *matcher) {
    if (matcher == NULL)
        return;
    frizbee_matcher_free(matcher->frizbee);
    free(matcher);
}
/* Map a Frizbee score onto the portable scale; a match is always positive. */
static int mapped_score(const tl_fuzzy_matcher *matcher, uint16_t score) {
    int value = MATCHER_OFFSET + (int)score;
    return value < 1 ? 1 : value > matcher->bound ? matcher->bound : value;
}
tl_status fuzzy_matcher_score(tl_fuzzy_matcher *matcher, tl_text text, const char *ascii,
                              int *out) {
    if (matcher == NULL || out == NULL || text.symbols == NULL || text.boundaries == NULL)
        return TL_INVALID;
    *out = 0;
    tl_text word = matcher->word;
    if (word.length > text.length || (text.mask & word.mask) != word.mask)
        return TL_OK;
    if (matcher->frizbee == NULL)
        return fuzzy_score(text, word, out);
    size_t length = text.length;
    bool plain = true;
    if (ascii == NULL) {
        if (!encode_text(text, matcher->scratch, sizeof(matcher->scratch), &length, &plain))
            return fuzzy_score(text, word, out);
        ascii = matcher->scratch;
    } else if (length > FUZZY_MATCHER_MAX_BYTES) {
        return fuzzy_score(text, word, out);
    }
    /* ASCII case-insensitive membership equals the tokenizer's; for other text
     * the symbols stay the authority and the portable scorer covers the rare
     * case-mapping disagreement. */
    if (!plain && !ordered_match(text, word))
        return TL_OK;
    frizbee_match_t match;
    if (frizbee_match_one(matcher->frizbee, (frizbee_str_t){.ptr = ascii, .len = length}, 0,
                          &match)) {
        *out = mapped_score(matcher, match.score);
        return TL_OK;
    }
    return plain ? TL_OK : fuzzy_score(text, word, out);
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
