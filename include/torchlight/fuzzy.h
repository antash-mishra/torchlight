/* Pure scoring of normalized ordered subsequences and bounded edit distance. */
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
/* Largest edit distance fuzzy_edit_distance resolves exactly. */
#define FUZZY_EDIT_LIMIT 1
/** Optimal-string-alignment distance between two symbol views, where an
 * insertion, deletion, substitution or swap of adjacent symbols each costs one.
 * Writes the distance when it is at most FUZZY_EDIT_LIMIT, otherwise
 * FUZZY_EDIT_LIMIT + 1. Views are borrowed; only symbols/length are read.
 * TL_INVALID for NULL out or NULL symbols in a nonempty view; no allocation. */
tl_status fuzzy_edit_distance(tl_text a, tl_text b, size_t *out);
#endif
