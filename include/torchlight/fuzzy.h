/* Scoring of normalized ordered subsequences (a portable scorer and the
 * Frizbee SIMD matcher on the same scale) and bounded edit distance. */
#ifndef TORCHLIGHT_FUZZY_H
#define TORCHLIGHT_FUZZY_H
#include "torchlight/tokenize.h"
/* Bounded stack scratch; larger text uses the greedy baseline. */
#define FUZZY_OPTIMAL_MAX_SYMBOLS 512
/** Optimally score a nonempty query in text, writing positive score or zero for
 * no match. Views are borrowed. TL_INVALID for bad views/out; no allocations.
 * For text above FUZZY_OPTIMAL_MAX_SYMBOLS use the greedy fallback.
 * Boundary/consecutive bonuses favor abbreviations; basename priority is a
 * responsibility of the orchestrator. This is not an edit-distance scorer. */
tl_status fuzzy_score(tl_text text, tl_text query, int *out);
/** Earliest-alignment comparison baseline, with the same borrowed views,
 * output, validation and allocation-free contract as fuzzy_score. */
tl_status fuzzy_score_greedy(tl_text text, tl_text query, int *out);
/** Upper bound of any fuzzy_score result for a query of query_length symbols,
 * saturating at INT_MAX. Pure; no errors. */
int fuzzy_score_bound(size_t query_length);
/* Words longer than this keep the portable scorer: Frizbee's scratch grows
 * with the word, and such words are rare in typed queries. */
#define FUZZY_MATCHER_MAX_SYMBOLS 64
/* Texts whose UTF-8 encoding exceeds this keep the portable scorer, because
 * Frizbee's own fallback for longer texts allocates per call. */
#define FUZZY_MATCHER_MAX_BYTES 1024
typedef struct tl_fuzzy_matcher tl_fuzzy_matcher;
/** Compile a nonempty normalized word for repeated scoring with Frizbee's SIMD
 * Smith-Waterman matcher, weighted to mirror fuzzy_score. The word's symbols
 * are copied; the view need not outlive the matcher. Words with opaque
 * (invalid UTF-8) symbols or more than FUZZY_MATCHER_MAX_SYMBOLS symbols get a
 * matcher that scores with fuzzy_score. Allocation happens here only, which is
 * the bounded exception to allocation-free queries: once per query word and
 * scoring thread, never per scored text. A matcher is not thread-safe; use one
 * per thread. Free with fuzzy_matcher_destroy. TL_INVALID for a bad view or
 * NULL out, TL_NOMEM; out is NULL on error. Allocation failure inside Frizbee
 * aborts the process (Rust's allocation-error behavior). */
tl_status fuzzy_matcher_create(tl_text word, tl_fuzzy_matcher **out);
/** Free a matcher and its Frizbee state; NULL allowed. */
void fuzzy_matcher_destroy(tl_fuzzy_matcher *matcher);
/** Score the matcher's word in text on fuzzy_score's scale: positive exactly
 * when the word is an ordered subsequence of text's symbols, at most
 * fuzzy_score_bound(word length), zero otherwise. ascii is optional: when
 * non-NULL it holds text.length raw bytes, all below 0x80, whose ASCII lower
 * case equals text.symbols (case is kept for camelCase bonuses). Otherwise the
 * symbols are encoded as UTF-8 into the matcher's scratch. Texts Frizbee cannot
 * take (opaque symbols, encodings over FUZZY_MATCHER_MAX_BYTES) use
 * fuzzy_score. Views are borrowed. No allocation. TL_INVALID for NULL matcher
 * or out, or a text view without symbols/boundaries. */
tl_status fuzzy_matcher_score(tl_fuzzy_matcher *matcher, tl_text text, const char *ascii, int *out);
/* Largest edit distance fuzzy_edit_distance resolves exactly. */
#define FUZZY_EDIT_LIMIT 1
/** Optimal-string-alignment distance between two symbol views, where an
 * insertion, deletion, substitution or swap of adjacent symbols each costs one.
 * Writes the distance when it is at most FUZZY_EDIT_LIMIT, otherwise
 * FUZZY_EDIT_LIMIT + 1. Views are borrowed; only symbols/length are read.
 * TL_INVALID for NULL out or NULL symbols in a nonempty view; no allocation. */
tl_status fuzzy_edit_distance(tl_text a, tl_text b, size_t *out);
#endif
