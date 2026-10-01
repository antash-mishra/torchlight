/* Conservative character-mask filter and ordered subsequence matching. */
#ifndef TORCHLIGHT_SUBSEQ_H
#define TORCHLIGHT_SUBSEQ_H
#include "torchlight/tokenize.h"
/** Test normalized query against text; borrowed views, no allocation/errors.
 * Empty query matches. NULL symbols in a nonempty view return false. */
bool subseq_matches(tl_text text, tl_text query);
#endif
