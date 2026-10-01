/* Pure scoring of normalized ordered subsequences. */
#ifndef TORCHLIGHT_FUZZY_H
#define TORCHLIGHT_FUZZY_H
#include "torchlight/tokenize.h"
/** Greedily score a nonempty query in text, writing positive score or zero for
 * no match. Views are borrowed. TL_INVALID for bad views/out; no allocations.
 * Boundary/consecutive bonuses favor abbreviations; basename priority is a
 * responsibility of the orchestrator. This is not an edit-distance scorer. */
tl_status fuzzy_score(tl_text text, tl_text query, int *out);
#endif
