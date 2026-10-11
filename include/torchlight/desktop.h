/* Resident XDG desktop catalog; discovery and publication run off the query thread. */
#ifndef TORCHLIGHT_DESKTOP_H
#define TORCHLIGHT_DESKTOP_H
#include "torchlight/lexical.h"
#define DESKTOP_ID_BASE (UINT64_C(1) << 62)
#define DESKTOP_MAX_ENTRIES 8192
/* Longest StartupWMClass kept; longer values are treated as absent. */
#define DESKTOP_WM_CLASS_BYTES 256
typedef struct tl_desktop tl_desktop;
typedef struct {
    uint64_t id, revision;
    const char *desktop_id, *filename, *name, *icon;
    const char *generic_name, *keywords;
    /* StartupWMClass naming the entry's windows; NULL when absent or too long. */
    const char *wm_class;
    bool settings;
} tl_desktop_entry;
/* Refresh timing in milliseconds; zero selects the default. Every check_ms
 * (default 1000) the worker compares a cheap change stamp of the applications
 * trees (which .desktop files exist and the stat of each) and parses them only
 * when it changed, once more settle_ms (default 5000) after the last change,
 * and at least every rescan_ms (default 600000): a TryExec binary appearing on
 * PATH changes no stamp. */
typedef struct {
    unsigned check_ms, settle_ms, rescan_ms;
} tl_desktop_options;
/** Create owned XDG catalog and refresh worker; options (NULL for defaults)
 * are copied. Environment/locale must remain stable during its lifetime.
 * TL_INVALID/NOMEM/IO/LIMIT; out NULL on error. */
tl_status desktop_create(const tl_desktop_options *options, tl_desktop **out);
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
/** desktop_query with personal boosts for entries at sorted-id positions of
 * the leased snapshot (desktop_entry; remap after desktop_gen changes), as in
 * lexical_workspace_boost, including its errors. Boosts apply to this query
 * only; NULL boosts with zero count gives desktop_query. Requires the lease.
 * No I/O or allocation. */
tl_status desktop_query_boosted(tl_desktop *desktop, const char *query,
                                const tl_lexical_boost *boosts, size_t boost_count,
                                tl_result *results, size_t capacity, size_t *count);
/** Resolve session-scoped id in current snapshot; NULL when stale. Requires
 * lease; borrowed entry/strings valid until release. No I/O/allocation. */
const tl_desktop_entry *desktop_resolve(const tl_desktop *desktop, uint64_t id);
/** Request a parse at the next check even without a changed stamp; thread-safe,
 * no I/O. Changed files are noticed by the worker's checks without it. */
void desktop_refresh(tl_desktop *desktop);
/** Count of finished discoveries, successful or not (each reads and parses
 * every .desktop file), including the one desktop_create runs. For tests and
 * diagnostics: an idle catalog stops counting. Thread-safe; zero for NULL; no
 * errors. */
uint64_t desktop_parses(tl_desktop *desktop);
/** Borrow current snapshot sequence/count while holding an exclusive query
 * lease. Zero for NULL; no allocation/I/O or errors. Sequence starts at one. */
uint64_t desktop_gen(const tl_desktop *desktop);
size_t desktop_count(const tl_desktop *desktop);
/** Borrow entry at sorted-id position under a query lease, NULL if absent.
 * Strings (including generic_name/keywords) borrow lease. No allocation/I/O. */
const tl_desktop_entry *desktop_entry(const tl_desktop *desktop, size_t position);
#endif
