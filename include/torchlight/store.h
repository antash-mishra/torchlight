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
/** Open/create owned catalog at path, enabling WAL/foreign keys and schema v1.
 * out NULL on error: TL_INVALID, TL_NOMEM, TL_IO or TL_STATE (unknown schema).
 * Parent directory must exist. Connections are serialized by their caller. */
tl_status store_create(const char *path, tl_store **out);
/** Close catalog, rolling back unfinished work; NULL allowed, no errors. */
void store_destroy(tl_store *store);
/** Start a full-scope scan transaction with empty seen membership. TL_STATE if
 * active; TL_INVALID for NULL, TL_IO on SQL error. */
tl_status store_begin(tl_store *store);
/** Record a visited entry. With stat data, upsert borrowed entry bytes,
 * preserving the id and invalidating embeddings when metadata changed; without
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
#endif
