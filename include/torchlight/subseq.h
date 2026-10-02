/* Conservative character-mask filter, ordered subsequence matching, and a
 * cache of complete subsequence membership for incremental narrowing. */
#ifndef TORCHLIGHT_SUBSEQ_H
#define TORCHLIGHT_SUBSEQ_H
#include "torchlight/tokenize.h"
/* Longest word (in symbols) the membership cache records. */
#define SUBSEQ_CACHE_MAX_SYMBOLS 1024
typedef struct tl_subseq_cache tl_subseq_cache;
/** Test normalized query against text; borrowed views, no allocation/errors.
 * Empty query matches. NULL symbols in a nonempty view return false. */
bool subseq_matches(tl_text text, tl_text query);
/** Create an owned, empty cache able to hold capacity members (slots).
 * TL_INVALID for NULL out, TL_NOMEM/TL_LIMIT; out NULL on failure. */
tl_status subseq_cache_create(size_t capacity, tl_subseq_cache **out);
/** Free the cache; NULL allowed. */
void subseq_cache_destroy(tl_subseq_cache *cache);
/** If the cached word is a symbol prefix of word and was recorded with the same
 * caller-defined mode, borrow its complete membership (valid until the next
 * begin/commit/destroy) and return true. Because a subsequence of an extended
 * word is a subsequence of its prefix, those members are the only slots that
 * can match word. Returns false otherwise or for NULL arguments. */
bool subseq_cache_lookup(const tl_subseq_cache *cache, tl_text word, int mode,
                         const uint32_t **members, size_t *count);
/** Start recording membership for word/mode into a writable buffer of the
 * create capacity. It never aliases the members borrowed from lookup, so a
 * scan may read the old membership while writing the new one. TL_LIMIT for
 * words over SUBSEQ_CACHE_MAX_SYMBOLS, TL_INVALID for bad arguments. */
tl_status subseq_cache_begin(tl_subseq_cache *cache, tl_text word, int mode, uint32_t **buffer);
/** Make the count members written since begin the cached membership. Callers
 * must record every matching slot: a partial list would lose matches later.
 * TL_STATE without begin, TL_LIMIT if count exceeds capacity. */
tl_status subseq_cache_commit(tl_subseq_cache *cache, size_t count);
#endif
