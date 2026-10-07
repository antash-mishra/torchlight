/* SQLite catalog API: SQL stays entirely inside the storage module. */
#ifndef TORCHLIGHT_STORE_H
#define TORCHLIGHT_STORE_H
#include "torchlight/crawl.h"
typedef struct tl_store tl_store;
typedef struct {
    uint64_t id;
    const char *path;
    bool is_root, is_dir;
} tl_store_entry;
typedef tl_status (*tl_store_callback)(void *context, const tl_store_entry *entry);
/** Receive one registered root path, borrowed until the callback returns. */
typedef tl_status (*tl_store_root_callback)(void *context, const char *root);
/** Open/create owned catalog at path, enabling WAL/foreign keys and schema v3.
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
/** Persist accepted desktop launch once by desktop id, independent of file rows.
 * Strings borrowed during call. Same errors/idempotency as store_open_event. */
tl_status store_desktop_open(tl_store *store, const char *event_id, const char *desktop_id,
                             const char *search_id, int64_t timestamp);
/** Prune unseen entries in canonical absolute root, then persist root status.
 * Entries under unreadable scopes, and under other registered roots nested in
 * root that this scan did not visit, are kept. Call only after a scan that
 * reached the end; no other scopes are pruned. Same errors as store_put. root
 * is borrowed for this call; transaction remains active. */
tl_status store_prune(tl_store *store, const char *root);
/** After a children-only crawl_scope of directory in this transaction, delete
 * its direct children (with their subtrees) that the transaction did not see.
 * Entries at or below kept paths and registered roots nested in directory are
 * spared; directory itself is never deleted. Uses a path-index range, never a
 * whole-catalog scan. TL_STATE outside a transaction, TL_INVALID for NULL or
 * relative paths, TL_LIMIT/NOMEM/IO. directory is borrowed for the call. */
tl_status store_prune_children(tl_store *store, const char *directory);
/** After a recursive crawl_scope of directory, delete entries strictly below
 * it that the transaction did not see, sparing the same scopes. Same errors. */
tl_status store_prune_tree(tl_store *store, const char *directory);
/** Record the ids of files rows that later catalog transactions insert,
 * update or delete, keeping at most limit ids (zero disables tracking).
 * TL_INVALID for NULL, TL_STATE inside a transaction. */
tl_status store_track_changes(tl_store *store, size_t limit);
/** After the last catalog transaction ended, borrow its touched row ids sorted
 * ascending without duplicates (valid until the next store_begin). complete is
 * false when tracking is off or more than its limit changed, so the caller
 * must reload everything; roots_changed reports registered-root changes.
 * TL_INVALID for NULL, TL_STATE during a transaction. No allocation. */
tl_status store_changes(tl_store *store, const uint64_t **ids, size_t *count, bool *complete,
                        bool *roots_changed);
/** Stream, from one read snapshot, the committed rows whose ids are listed
 * (ascending), skipping ids without a row (deleted), plus that snapshot's
 * catalog_gen. Same borrowed-path and error contract as store_load_catalog;
 * TL_INVALID also for ids beyond INT64_MAX. */
tl_status store_load_ids(tl_store *store, const uint64_t *ids, size_t count,
                         tl_store_callback callback, void *context, uint64_t *out_catalog_gen);
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
/** Stage a complete UTF-8 model/transform descriptor and clear cache marks.
 * gen is nonzero and descriptor borrowed, <=4096 bytes. Never advances
 * catalog_gen. Background-only; no active catalog transaction. TL_INVALID/IO/
 * STATE. Staged cache survives restart but is never interpreted as active. */
tl_status store_embedding_stage(tl_store *store, uint64_t emb_gen, const char *descriptor);
/** Read/touch an exact prepared-text cache key in one emb_gen. Copy finite
 * normalized floats into caller buffer; out_found false for missing/wrong-sized/corrupt
 * cache. No ownership transfer. Background-only. TL_INVALID/IO/STATE. */
tl_status store_embedding_get(tl_store *store, uint64_t emb_gen, const char *text, float *values,
                              size_t dimensions, bool *out_found);
/** Persist finite normalized floats under exact prepared text/model version.
 * All pointers borrowed; dimensions 1..4096. Background-only, no catalog_gen
 * change. TL_INVALID/IO/STATE. Uses little-endian float32 cache payload. */
tl_status store_embedding_put(tl_store *store, uint64_t emb_gen, const char *text,
                              const float *values, size_t dimensions);
/** Atomically activate staged descriptor and discard unmarked/other-model
 * cache rows. Call only after complete resident staging validates. Existing
 * query snapshots remain owned in RAM. Background-only. TL_INVALID/IO/STATE. */
tl_status store_embedding_activate(tl_store *store, uint64_t emb_gen);
/** Begin/end a bounded background cache batch, amortizing WAL commits.
 * Use this connection only for embedding get/put until end; false rolls back.
 * No ownership transfer. TL_INVALID/IO/STATE; end always clears batch state.
 * Never advances catalog_gen. Stop batching before stage/activate. */
tl_status store_embedding_batch_begin(tl_store *store);
tl_status store_embedding_batch_end(tl_store *store, bool commit);
#endif
