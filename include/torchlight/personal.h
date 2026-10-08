/* M5 personal ranking state for the daemon's search thread (ADR 0033): the
 * live usage summary, the queries of recent searches, recently recorded
 * launch events, and per-query boosts for the file catalog and the desktop
 * engine. Only the search thread calls it, so nothing here locks; the item
 * count is also published atomically for status reports from any thread. */
#ifndef TORCHLIGHT_PERSONAL_H
#define TORCHLIGHT_PERSONAL_H
#include "torchlight/catalog.h"
#include "torchlight/desktop.h"
#include "torchlight/usage.h"
/* Opens record the query of one of these recent searches: a popup opens a
 * result of one of its latest searches, and a few dozen cover several
 * typing clients. Retried launches repeat one of the recent event ids. */
#define PERSONAL_RECENT_SEARCHES 32
#define PERSONAL_RECENT_EVENTS 32
typedef struct tl_personal tl_personal;

/** Create owned state with an empty live summary whose entries stop counting
 * after retention_seconds (positive). TL_INVALID/TL_NOMEM; out NULL on
 * failure. Release with personal_destroy. */
tl_status personal_create(int64_t retention_seconds, tl_personal **out);
/** Free state and its summary; NULL allowed. */
void personal_destroy(tl_personal *personal);
/** Adopt the summary rebuilt from saved history at startup, taking ownership
 * of loaded (NULL ignored): the opens recorded since startup are merged into
 * it and it becomes the live summary. Discarded when history was cleared
 * after startup, because it predates the clear. */
void personal_adopt(tl_personal *personal, tl_usage *loaded);
/** Remember the query (borrowed, copied) of search_id for later opens,
 * replacing the oldest remembered search. Ignores NULL or oversized input. */
void personal_remember(tl_personal *personal, const char *search_id, const char *query);
/** Borrow the remembered query of search_id, or NULL when unknown. Valid
 * until the next personal_remember or personal_clear. */
const char *personal_query_of(const tl_personal *personal, const char *search_id);
/** Count one accepted open of target at now, opened from query (NULL for
 * none). A repeated event_id among the recent ones counts once, as the saved
 * history does. TL_INVALID for NULL state, target or event id; usage_record
 * errors otherwise (TL_LIMIT: a desktop id too long to keep). */
tl_status personal_record(tl_personal *personal, const char *event_id, tl_usage_target target,
                          const char *query, int64_t now);
/** Forget the summary, remembered searches and events (history clearing).
 * A startup summary adopted later is discarded. NULL ignored. */
void personal_clear(tl_personal *personal);
/** Compute boosts for query at now. *files receives catalog boosts for file
 * ids (keyed by the summary's version, so a lease maps ids once per change);
 * *apps borrows boosts for entries of the leased desktop snapshot, remapped
 * when its desktop_gen changes (desktop NULL gives none). Both borrow state
 * until the next call. Counts are zero without usage. TL_INVALID for NULL
 * outputs; usage_boosts errors otherwise. Desktop must be leased. */
tl_status personal_boosts(tl_personal *personal, const tl_desktop *desktop, const char *query,
                          int64_t now, tl_catalog_boosts *files, const tl_lexical_boost **apps,
                          size_t *app_count);
/** Number of items in the live summary; safe from any thread. Zero for NULL. */
size_t personal_items(const tl_personal *personal);
#endif
