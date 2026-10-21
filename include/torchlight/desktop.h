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
/** Set named[i] for each of count results of a desktop query on this lease:
 * whether the query names that application, that is, every word of query
 * (split at whitespace, as lexical_query splits it) starts the application's
 * name, a word of the name or the name's initials. That is the evidence the
 * name bonus weights; generic names, keywords, typos and scattered letters do
 * not name an application. The daemon lists named applications before files
 * (ADR 0058). Ids not in the leased snapshot and queries without words are
 * not named. Requires the lease; uses the catalog's query scratch, so one
 * lease holder at a time. TL_INVALID for NULL arguments (count zero excepted),
 * TL_LIMIT for a query over LEXICAL_QUERY_BYTES; named is all false on error.
 * No I/O or allocation. */
tl_status desktop_named(tl_desktop *desktop, const char *query, const tl_result *results,
                        size_t count, bool *named);
/* What the words of a query find in an application, weakest first. */
typedef enum {
    /* Only typos, shared trigrams or scattered letters. */
    DESKTOP_FUZZY,
    /* Each word starts the name, a word of it or its initials, or starts a
     * word of the generic name or keywords. */
    DESKTOP_WORD_START,
    /* As word start, and each word that does not start the name is a whole
     * word of the generic name or keywords: the application is tagged with
     * it ("audio" for Sound, not "aud"). */
    DESKTOP_DESCRIBED,
    /* Each word starts the name, a word of it or its initials (desktop_named). */
    DESKTOP_NAMED
} tl_desktop_evidence;
/** Set evidence[i] for each of count results of a desktop query on this lease:
 * what the query's words (split at whitespace, as lexical_query splits them)
 * find in that application. The generic name field also holds the untranslated
 * name. The daemon lists named applications first, described ones after exact
 * file names, and for a one-word query drops fuzzy matches when any result
 * matches by a word start (ADR 0061). Ids not in the leased snapshot and
 * queries without words are fuzzy. Requires the lease; uses the catalog's
 * query scratch, so one lease holder at a time. TL_INVALID for NULL arguments
 * (count zero excepted), TL_LIMIT for a query over LEXICAL_QUERY_BYTES;
 * evidence is all fuzzy on error. No I/O or allocation. */
tl_status desktop_evidence(tl_desktop *desktop, const char *query, const tl_result *results,
                           size_t count, tl_desktop_evidence *evidence);
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
/** Whether the running desktop's own application menu lists an entry, judged
 * by its OnlyShowIn and NotShowIn lists (NULL-terminated, NULL when the key is
 * absent) and its Exec line (NULL when absent). desktops is an
 * XDG_CURRENT_DESKTOP value: colon-separated names, NULL or empty for none.
 * The rule is GIO's: the first of desktops that either list names decides,
 * otherwise an entry is listed only without OnlyShowIn. On Cinnamon (desktops
 * names X-Cinnamon) it is the Cinnamon menu's: an OnlyShowIn naming GNOME also
 * lists the entry, and an Exec line starting with gnome-control-center never
 * does. Hidden, NoDisplay and TryExec are the caller's. Both the catalog and
 * the popup's launch check use it (ADR 0057). Borrowed input; no allocation,
 * I/O or errors. */
bool desktop_shown_in(const char *const *only_show_in, const char *const *not_show_in,
                      const char *exec, const char *desktops);
#endif
