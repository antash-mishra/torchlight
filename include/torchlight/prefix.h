/* Sorted prefix candidate channel over borrowed normalized keys. */
#ifndef TORCHLIGHT_PREFIX_H
#define TORCHLIGHT_PREFIX_H
#include "torchlight/tokenize.h"
/* Hit scores by key kind. Basename keys outrank tokens and initials; tokens
 * before text.basename (parent directories) score lowest. */
enum {
    PREFIX_BASENAME_SCORE = 6000,
    PREFIX_TOKEN_SCORE = 5000,
    PREFIX_COMPLETE_BONUS = 128,
    PREFIX_INITIALS_SCORE = 4500,
    PREFIX_PARENT_SCORE = 1000
};
typedef struct tl_prefix tl_prefix;
/** Receive one key hit for slot; a slot may be reported once per matching key.
 * Return TL_OK to continue; any other status stops the query and is returned. */
typedef tl_status (*tl_prefix_hit)(void *context, size_t slot, int score);
/** Create an owned empty index; out NULL on invalid/allocation failure. */
tl_status prefix_create(tl_prefix **out);
/** Destroy index and its owned initials; borrowed text is never freed. NULL allowed. */
void prefix_destroy(tl_prefix *index);
/** Add the basename, every token and the basename initials of text for slot.
 * text storage must not move or be freed while the index lives. Before finish
 * only; failure may partially add keys, so discard the index. TL_LIMIT for
 * slots or lengths beyond UINT32_MAX. */
tl_status prefix_add(tl_prefix *index, tl_text text, size_t slot);
/** Sort keys and seal index; TL_INVALID for NULL, TL_STATE if already sealed,
 * TL_NOMEM if the sorted key table cannot be allocated (discard the index). */
tl_status prefix_finish(tl_prefix *index);
/** Report every key that starts with the nonempty query, in key order. Never
 * truncates. Complete token keys gain PREFIX_COMPLETE_BONUS. No allocation or I/O. TL_INVALID for
 * bad arguments, TL_STATE before finish, or the first non-OK status returned by hit. */
tl_status prefix_query(const tl_prefix *index, tl_text query, tl_prefix_hit hit, void *context);
#endif
