/* SQLite catalog API: SQL stays entirely inside the storage module. */
#ifndef TORCHLIGHT_STORE_H
#define TORCHLIGHT_STORE_H
#include "torchlight/crawl.h"
typedef struct tl_store tl_store;
typedef struct {
    uint64_t id;
    const char *path;
    bool is_root;
} tl_store_entry;
typedef tl_status (*tl_store_callback)(void *context, const tl_store_entry *entry);
/** Receive one registered root path, borrowed until the callback returns. */
typedef tl_status (*tl_store_root_callback)(void *context, const char *root);
/** Open/create owned catalog at path, enabling WAL/foreign keys and schema v2.
 * Migrate v1 atomically, retaining ids/history; legacy identities start unknown.
 * out NULL on error: TL_INVALID, TL_NOMEM, TL_IO or TL_STATE (unknown schema).
 * Parent directory must exist. Connections are serialized by their caller. */
tl_status store_create(const char *path, tl_store **out);
/** Close catalog, rolling back unfinished work; NULL allowed, no errors. */
void store_destroy(tl_store *store);
/** Start a full-scope scan transaction with empty seen membership. TL_STATE if
 * active; TL_INVALID for NULL, TL_IO on SQL error. */
tl_status store_begin(tl_store *store);
/** Record a visited entry. With stat data, upsert borrowed entry bytes,
 * preserving the id for the same filesystem incarnation and invalidating
 * embeddings when metadata changed. A changed identity retires the row and
 * descendants, allocating fresh ids; unknown legacy identities are adopted.
 * Birth time detects reused inodes; ctime fallback conservatively retires ids
 * on metadata changes too. Without
 * it, leave any saved row as is. An unreadable entry also keeps every saved
 * entry at or below its path from store_prune in this scan. Only inside a
 * transaction. TL_INVALID for malformed entries, TL_IO for SQL, TL_LIMIT for
 * byte lengths beyond SQLite's limit, TL_STATE outside scan. */
tl_status store_put(tl_store *store, const tl_crawl_entry *entry);
/** Prune unseen entries in canonical absolute root, then persist root status.
 * Entries under unreadable scopes, and under other registered roots nested in
 * root that this scan did not visit, are kept. Call only after a scan that
 * reached the end; no other scopes are pruned. Same errors as store_put. root
 * is borrowed for this call; transaction remains active. */
tl_status store_prune(tl_store *store, const char *root);
/** Keep every saved entry at or below path from pruning/forgetting in this
 * transaction (e.g. a configured root that is currently unavailable). TL_STATE
 * outside a transaction, TL_INVALID for relative paths, TL_IO for SQL. */
tl_status store_keep(tl_store *store, const char *path);
/** Unregister root and delete its saved entries that no scan in this
 * transaction saw or kept (the root was removed from the configuration).
 * Same errors as store_prune; the transaction remains active. */
tl_status store_forget_root(tl_store *store, const char *root);
/** Commit catalog changes and increment persisted catalog_gen; TL_STATE outside
 * transaction, TL_IO on failure, TL_LIMIT when catalog_gen reaches INT64_MAX
 * (transaction remains available for rollback). Metadata must be valid decimal. */
tl_status store_commit(tl_store *store);
/** Roll back active transaction; TL_INVALID/STATE/IO on errors. */
tl_status store_rollback(tl_store *store);
/** Stream catalog in ascending id order. Callback borrows path until return;
 * copy it if retaining. Callback errors propagate, plus TL_INVALID/IO/NOMEM.
 * No queries may be active on this connection during callback. */
tl_status store_load(tl_store *store, tl_store_callback callback, void *context);
/** Stream committed rows and their catalog_gen from one SQLite read snapshot.
 * out_catalog_gen is zero on error, including callback errors; partial callback
 * output must be discarded then. Same borrowed-path contract as store_load.
 * TL_STATE during a scan/read transaction; TL_IO for corrupt/missing metadata,
 * plus TL_INVALID/NOMEM. Callback must not use this connection. Other
 * connections may commit meanwhile; rows and catalog_gen stay consistent.
 * No schema change; intended for background resident snapshot construction. */
tl_status store_load_catalog(tl_store *store, tl_store_callback callback, void *context,
                             uint64_t *out_catalog_gen);
/** Stream registered roots in byte order. Callback errors propagate, plus
 * TL_INVALID/TL_IO/TL_NOMEM. No queries may run on this connection meanwhile. */
tl_status store_roots(tl_store *store, tl_store_root_callback callback, void *context);
/** Stream the pending scan view and next catalog_gen before commit. Requires
 * an active write transaction. Same callback lifetime as store_load_catalog;
 * discard partial output on errors. TL_INVALID/STATE/IO/LIMIT plus callback
 * errors. Prepares a resident candidate without publishing uncommitted data. */
tl_status store_prepare_catalog(tl_store *store, tl_store_callback callback, void *context,
                                uint64_t *out_catalog_gen);
/** Report whether files/roots changed in the active scan, excluding temporary
 * membership and sequence bookkeeping. TL_INVALID/STATE; no ownership. */
tl_status store_catalog_changed(tl_store *store, bool *out);
/** Move an exact byte path and descendants in an active transaction, preserving
 * ids, replacing destination rows and invalidating embeddings. Paths must be
 * absolute, distinct and neither within the other; caller validates eligibility
 * using crawl policy. Missing source is a no-op. TL_INVALID/STATE/IO/NOMEM/LIMIT. */
tl_status store_move(tl_store *store, const char *old_path, const char *new_path);
/** Persist a search id/query at Unix timestamp. Idempotent for the same id;
 * conflicting retries return TL_STATE. Borrowed nonempty UTF-8 strings. History
 * operations require no active catalog transaction and never advance catalog_gen.
 * TL_INVALID/STATE/IO/LIMIT. */
tl_status store_search(tl_store *store, const char *id, const char *query, int64_t timestamp);
/** Persist an accepted open event exactly once. Missing/deleted file returns
 * TL_STATE. Absent retained search is recorded as NULL. Conflicting retries
 * return TL_STATE; TL_INVALID/STATE/IO/LIMIT. All strings borrowed. */
tl_status store_open_event(tl_store *store, const char *event_id, uint64_t file_id,
                           const char *search_id, int64_t timestamp);
/** Delete history older than cutoff, or all history when clear is true. Applies
 * to opens and searches, never catalog rows/gen. TL_INVALID/STATE/IO. */
tl_status store_history_prune(tl_store *store, int64_t cutoff, bool clear);
#endif
