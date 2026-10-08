/* Resident personal usage summary for M5 ranking (ADR 0033): capped,
 * exponentially decayed open counts per file id and per application desktop
 * id, plus query-to-open pairs, turned into bounded per-query boosts. It is a
 * single-owner structure without locks: the daemon's search thread owns the
 * live summary, and the persistence thread builds the startup copy. */
#ifndef TORCHLIGHT_USAGE_H
#define TORCHLIGHT_USAGE_H
#include "torchlight/common.h"
#include <stdbool.h>
/* Bounds per-keystroke side-pass work and memory; a person opens far fewer
 * distinct items within the history retention. */
#define USAGE_MAX_ITEMS 2048
#define USAGE_MAX_PAIRS 4096
/* Stored queries keep this many normalized symbols; longer typed queries get
 * no query-to-open boost. Desktop ids at or above the byte limit are skipped. */
#define USAGE_QUERY_SYMBOLS 32
#define USAGE_DESKTOP_ID_BYTES 128
/* Score units added to lexical scores. Frecency stays below the 500-point gap
 * between initials (4500) and token prefixes (5000), so it reorders matches of
 * similar strength; query-to-open history may cross about one channel tier.
 * Both together stay within LEXICAL_BOOST_MAX, below the exact-name tier. */
#define USAGE_FRECENCY_BOOST_MAX 400
#define USAGE_QUERY_BOOST_MAX 1200
/* An open's weight halves every two weeks. */
#define USAGE_HALF_LIFE_SECONDS (14 * 24 * 3600)
typedef struct tl_usage tl_usage;
/* Exactly one key: a nonzero file id, or (file_id zero) a desktop id. */
typedef struct {
    uint64_t file_id;
    const char *desktop_id;
} tl_usage_target;

/** Create an empty owned summary whose items and pairs stop counting once
 * their last open is retention_seconds old (positive). TL_INVALID/TL_NOMEM;
 * out NULL on failure. Release with usage_destroy. */
tl_status usage_create(int64_t retention_seconds, tl_usage **out);
/** Free the summary; NULL allowed. */
void usage_destroy(tl_usage *usage);
/** Record one accepted open of target at timestamp (seconds, nonnegative),
 * opened from the results of query (NULL or blank for none). Copies the
 * desktop id and the normalized query; the caller keeps both. Timestamps may
 * arrive out of order. A full table evicts its weakest item or pair. Returns
 * TL_INVALID for NULL usage, a missing/double key or negative timestamp, and
 * TL_LIMIT for a desktop id of USAGE_DESKTOP_ID_BYTES or more (nothing is
 * recorded); an unnormalizable query records the item without a pair. */
tl_status usage_record(tl_usage *usage, tl_usage_target target, const char *query,
                       int64_t timestamp);
/** Forget every item and pair (history clearing). NULL ignored; no errors. */
void usage_clear(tl_usage *usage);
/** Add from's items and pairs into into, combining decayed weights, as if
 * every open recorded in from had been recorded in into. from is unchanged.
 * Evicts the weakest entries when full. TL_INVALID for NULL arguments. */
tl_status usage_merge(tl_usage *into, const tl_usage *from);
/** Counter that changes whenever item indices change (an item added,
 * evicted or cleared, or a merge); callers cache index mappings against it.
 * Zero for NULL. No errors. */
uint64_t usage_version(const tl_usage *usage);
/** Number of item indices, 0..USAGE_MAX_ITEMS; zero for NULL. */
size_t usage_count(const tl_usage *usage);
/** Borrow the key of item index (below usage_count); the desktop id stays
 * valid until the summary next changes. A zeroed target for invalid input. */
tl_usage_target usage_target(const tl_usage *usage, size_t index);
/** Compute each item's boost for query at time now: frecency (cached and
 * refreshed at most once a minute or after a change) plus query-to-open
 * history from stored queries that start with the normalized query. Items
 * past retention get zero and are evicted during a refresh, which changes
 * usage_version. *boosts borrows usage_count() values in
 * 0..USAGE_FRECENCY_BOOST_MAX + USAGE_QUERY_BOOST_MAX, valid until the next
 * call or change. Blank queries get frecency only. TL_INVALID for NULL
 * arguments. No allocation or I/O. */
tl_status usage_boosts(tl_usage *usage, const char *query, int64_t now, const int **boosts);
#endif
