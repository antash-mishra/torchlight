/* One-edit typo channel: a deletion-neighbourhood (SymSpell-style) index of
 * complete name tokens, verified with fuzzy_edit_distance. */
#ifndef TORCHLIGHT_TYPO_H
#define TORCHLIGHT_TYPO_H
#include "torchlight/tokenize.h"
/* Indexed tokens and queries must have this many symbols. Shorter words have
 * too many one-edit neighbours to be useful; longer ones are rare and would
 * multiply deletion keys. */
#define TYPO_MIN_SYMBOLS 3
#define TYPO_MAX_SYMBOLS 32
typedef struct tl_typo tl_typo;
typedef struct tl_typo_scratch tl_typo_scratch;
/** Receive a slot containing a token exactly one edit from the query. A slot
 * may be reported more than once. Return TL_OK to continue; any other status
 * stops the query and is returned. */
typedef tl_status (*tl_typo_hit)(void *context, size_t slot);
/** Create an owned empty index; out NULL on TL_INVALID/TL_NOMEM. */
tl_status typo_create(tl_typo **out);
/** Free the index; borrowed text is never freed. NULL allowed. Scratch objects
 * must be destroyed first. */
void typo_destroy(tl_typo *index);
/** Add text's tokens (split at separators and word boundaries) for slot.
 * Tokens of TYPO_MIN_SYMBOLS..TYPO_MAX_SYMBOLS symbols that are not all ASCII
 * digits are indexed. text symbols are borrowed and must outlive the index.
 * Before finish only. TL_INVALID/TL_STATE/TL_LIMIT/TL_NOMEM; on failure
 * discard the index. */
tl_status typo_add(tl_typo *index, tl_text text, size_t slot);
/** Deduplicate tokens, build deletion keys and seal. TL_INVALID/TL_STATE/
 * TL_NOMEM/TL_LIMIT; on failure discard the index. */
tl_status typo_finish(tl_typo *index);
/** Create owned per-query scratch for a sealed index (one per concurrent
 * querier). The index must outlive it. TL_INVALID/TL_STATE/TL_NOMEM. */
tl_status typo_scratch_create(const tl_typo *index, tl_typo_scratch **out);
/** Free scratch; NULL allowed. */
void typo_scratch_destroy(tl_typo_scratch *scratch);
/** Report every slot holding an indexed token at edit distance exactly one
 * from query (insertion, deletion, substitution or adjacent swap). Exact token
 * matches are left to the prefix channel. Queries outside the indexed length
 * range report nothing. No allocation or I/O. TL_INVALID for bad arguments or
 * foreign scratch, TL_STATE before finish, or the first non-OK hit status. */
tl_status typo_query(const tl_typo *index, tl_typo_scratch *scratch, tl_text query, tl_typo_hit hit,
                     void *context);
#endif
