/* Sorted prefix candidate channel over borrowed normalized keys. */
#ifndef TORCHLIGHT_PREFIX_H
#define TORCHLIGHT_PREFIX_H
#include "torchlight/tokenize.h"
typedef struct tl_prefix tl_prefix;
/** Create an owned empty index; out NULL on invalid/allocation failure. */
tl_status prefix_create(tl_prefix **out);
/** Destroy index and owned initials; borrowed text is never freed. NULL allowed. */
void prefix_destroy(tl_prefix *index);
/** Add all basename/path tokens for slot. text storage must outlive index.
 * Before finish only; failure may partially add keys, so discard the index. */
tl_status prefix_add(tl_prefix *index, tl_text text, size_t slot);
/** Sort keys and seal index; TL_INVALID for NULL, TL_STATE if already sealed. */
tl_status prefix_finish(tl_prefix *index);
/** Mark prefix matches by writing scores[slot] (maximum across keys). Buffers
 * are caller-owned; scores_count must cover every slot. No allocation or I/O.
 * TL_INVALID for bad arguments, TL_STATE before finish, TL_LIMIT for short buffer. */
tl_status prefix_query(const tl_prefix *index, tl_text query, int *scores, size_t scores_count);
#endif
