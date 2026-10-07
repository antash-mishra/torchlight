/* Relaxed-overlap trigram channel: finds names sharing enough three-symbol
 * substrings with a query, which tolerates typos and incomplete words. */
#ifndef TORCHLIGHT_TRIGRAM_H
#define TORCHLIGHT_TRIGRAM_H
#include "torchlight/tokenize.h"
/* Queries need this many distinct trigrams (five symbols); shorter words are
 * covered by prefix, subsequence and typo lookup. */
#define TRIGRAM_MIN_QUERY_TRIGRAMS 3
/* Longest query view (in symbols) the per-query scratch accepts. */
#define TRIGRAM_MAX_QUERY_SYMBOLS 1024
typedef struct tl_trigram tl_trigram;
typedef struct tl_trigram_scratch tl_trigram_scratch;
/** Receive a slot sharing `shared` of the query's `total` informative trigrams
 * (shared >= half of total). Return TL_OK to continue; any other status stops
 * the query and is returned. */
typedef tl_status (*tl_trigram_hit)(void *context, size_t slot, size_t shared, size_t total);
/** Create an owned empty index; out NULL on TL_INVALID/TL_NOMEM. */
tl_status trigram_create(tl_trigram **out);
/** Free the index; NULL allowed. Scratch objects must be destroyed first. */
void trigram_destroy(tl_trigram *index);
/** Index the distinct trigrams of text's symbols for slot. Slots must be
 * strictly increasing (gaps allowed). Before finish only. TL_INVALID for bad views or
 * decreasing slots, TL_STATE after finish, TL_LIMIT beyond 32-bit counts,
 * TL_NOMEM; on failure discard the index. text is not retained. */
tl_status trigram_add(tl_trigram *index, tl_text text, size_t slot);
/** Build posting lists and seal; TL_INVALID/TL_STATE/TL_NOMEM/TL_LIMIT. */
tl_status trigram_finish(tl_trigram *index);
/** Create owned per-query scratch for a sealed index (one per concurrent
 * querier). The index must outlive it. TL_INVALID/TL_STATE/TL_NOMEM. */
tl_status trigram_scratch_create(const tl_trigram *index, tl_trigram_scratch **out);
/** Free scratch; NULL allowed. */
void trigram_scratch_destroy(tl_trigram_scratch *scratch);
/** Report every slot sharing at least half (rounded up) of the query's
 * informative trigrams. Trigrams present in more than an eighth of slots are
 * ignored as uninformative; with a non-NULL reference index (sealed, borrowed)
 * the reference's postings decide that instead, so a small delta index
 * agrees with its base. Queries with fewer than TRIGRAM_MIN_QUERY_TRIGRAMS
 * distinct trigrams report nothing. Each slot is reported once, in the order
 * the shortest posting lists first reach it. No allocation or I/O. TL_INVALID for bad arguments or
 * scratch from another index, TL_STATE before finish, TL_LIMIT for queries over
 * TRIGRAM_MAX_QUERY_SYMBOLS, or the first non-OK status from hit. */
tl_status trigram_query(const tl_trigram *index, const tl_trigram *reference,
                        tl_trigram_scratch *scratch, tl_text query, tl_trigram_hit hit,
                        void *context);
#endif
