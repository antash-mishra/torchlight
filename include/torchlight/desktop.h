/* Resident XDG desktop catalog; discovery and publication run off the query thread. */
#ifndef TORCHLIGHT_DESKTOP_H
#define TORCHLIGHT_DESKTOP_H
#include "torchlight/lexical.h"
#define DESKTOP_ID_BASE (UINT64_C(1) << 62)
#define DESKTOP_MAX_ENTRIES 8192
typedef struct tl_desktop tl_desktop;
typedef struct {
    uint64_t id, revision;
    const char *desktop_id, *filename, *name, *icon;
    const char *generic_name, *keywords;
    bool settings;
} tl_desktop_entry;
/** Create owned XDG catalog and refresh worker. Environment/locale must remain
 * stable during its lifetime. TL_INVALID/NOMEM/IO/LIMIT; out NULL on error. */
tl_status desktop_create(tl_desktop **out);
/** Stop/join worker and free catalog. Release any lease first. NULL allowed. */
void desktop_destroy(tl_desktop *desktop);
/** Acquire a short exclusive query lease. Borrow entry strings until release;
 * no I/O/allocation. Call from one coordinator only, never recursively. */
void desktop_acquire(tl_desktop *desktop);
/** Release query lease. The worker reclaims old snapshots outside this lock. */
void desktop_release(tl_desktop *desktop);
/** Search resident localized names/generic names/keywords using lexical engine.
 * Requires lease; borrowed paths/entries valid until release. Empty query gives
 * no apps. Same capacity/errors as lexical_query, no I/O/allocation. */
tl_status desktop_query(tl_desktop *desktop, const char *query, tl_result *results, size_t capacity,
                        size_t *count);
/** Resolve session-scoped id in current snapshot; NULL when stale. Requires
 * lease; borrowed entry/strings valid until release. No I/O/allocation. */
const tl_desktop_entry *desktop_resolve(const tl_desktop *desktop, uint64_t id);
/** Request early refresh; thread-safe, no I/O. Changes also refresh every second. */
void desktop_refresh(tl_desktop *desktop);
/** Borrow current snapshot sequence/count while holding an exclusive query
 * lease. Zero for NULL; no allocation/I/O or errors. Sequence starts at one. */
uint64_t desktop_gen(const tl_desktop *desktop);
size_t desktop_count(const tl_desktop *desktop);
/** Borrow entry at sorted-id position under a query lease, NULL if absent.
 * Strings (including generic_name/keywords) borrow lease. No allocation/I/O. */
const tl_desktop_entry *desktop_entry(const tl_desktop *desktop, size_t position);
#endif
