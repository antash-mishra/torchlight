/* Reusable bounded worker pool with allocation-free range dispatch. */
#ifndef TORCHLIGHT_PARALLEL_H
#define TORCHLIGHT_PARALLEL_H
#include "torchlight/common.h"
#define PARALLEL_MAX_PARTICIPANTS 8
typedef struct tl_parallel tl_parallel;
/** Process disjoint [begin,end) indexes; context is borrowed until the dispatch
 * returns. Output elements in a range may be independently written. Return
 * TL_OK or an error propagated after every callback has finished. */
typedef tl_status (*tl_parallel_work)(void *context, size_t begin, size_t end);
/** Create an owned pool with 1..PARALLEL_MAX_PARTICIPANTS participants including
 * the caller. Workers start here and sleep between runs. out NULL on failure;
 * TL_INVALID/NOMEM/IO/OK. Uses the existing pthread dependency. */
tl_status parallel_create(size_t participants, tl_parallel **out);
/** Stop/join workers and free pool; NULL allowed. Finish all runs first and
 * never destroy from its callback. No errors. */
void parallel_destroy(tl_parallel *pool);
/** Process count indexes in a complete disjoint partition, skipping empty
 * ranges. Waits for every callback even on error. No allocation or I/O.
 * One coordinator per pool; no recursive dispatch on the same pool.
 * TL_INVALID for NULL pool/work, TL_STATE for overlapping runs, otherwise the
 * first callback error in participant order or TL_OK. */
tl_status parallel_run(tl_parallel *pool, size_t count, tl_parallel_work work, void *context);
#endif
