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
typedef enum {
    STORE_HISTORY_SEARCH,
    STORE_HISTORY_OPEN,
    STORE_HISTORY_DESKTOP_OPEN
} tl_store_history_kind;
/* One history record for store_history_write; strings are borrowed for the
 * call. A search needs search_id and query. An open needs event_id plus
 * file_id (STORE_HISTORY_OPEN) or desktop_id (STORE_HISTORY_DESKTOP_OPEN);
 * search_id optionally links its retained search, and a non-NULL query saves
 * that search row together with the open. */
typedef struct {
    tl_store_history_kind kind;
    const char *search_id, *query, *event_id, *desktop_id;
    uint64_t file_id;
    int64_t timestamp;
} tl_store_history_event;
/** Receive one retained open: a file id with NULL desktop_id, or file_id zero
 * with a desktop id; the query of its retained search or NULL; its Unix
 * timestamp. Strings are borrowed until return. */
typedef tl_status (*tl_store_open_callback)(void *context, uint64_t file_id, const char *desktop_id,
                                            const char *query, int64_t timestamp);
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
 * preserving the id (and embedding columns) for the same filesystem
 * incarnation. Refreshing an existing row's mtime/size, or adopting an
 * identity for it, counts as a metadata change (store_metadata_changed), not
 * a catalog change: no change id is recorded. A changed identity retires the row and
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
/** Report whether names changed in the active scan: files inserted, deleted,
 * moved, replaced or changed between file and directory, or roots changed.
 * Excludes metadata refreshes and temporary membership and sequence
 * bookkeeping. TL_INVALID/STATE; no ownership. */
tl_status store_catalog_changed(tl_store *store, bool *out);
/** Report whether the active scan refreshed metadata (mtime/size, identity
 * adoption) of rows whose names it kept. Worth committing so later scans stop
 * rewriting them, but no snapshot depends on it. TL_INVALID/STATE. */
tl_status store_metadata_changed(tl_store *store, bool *out);
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
/** Persist one history event (see tl_store_history_event) in a savepoint, so
 * an open and its search row land together or not at all. Inside a
 * store_history_begin batch it commits with the batch; otherwise on its own.
 * Idempotent per search id or event id; conflicting retries and opens of
 * missing files return TL_STATE without writing. Absent retained searches
 * link as NULL. TL_INVALID/LIMIT for malformed fields, TL_STATE during a
 * catalog scan or read, TL_IO for SQL. If SQLite rolls back the whole batch
 * (an I/O error, out of memory or a trigger), the batch ends: its earlier
 * events are lost, this returns TL_IO, and store_history_commit returns
 * TL_STATE. Insert statements are prepared once. */
tl_status store_history_write(tl_store *store, const tl_store_history_event *event);
/** Begin one write transaction for many history events (store_history_write),
 * so a drained queue costs one commit. TL_STATE while any transaction or read
 * is active, TL_INVALID for NULL, TL_IO for SQL. */
tl_status store_history_begin(tl_store *store);
/** Commit the history batch. A failed commit is rolled back and reported as
 * TL_IO: none of the batch's events persist. TL_STATE without a batch
 * (including one SQLite already rolled back), TL_INVALID for NULL. */
tl_status store_history_commit(tl_store *store);
/** Roll back the history batch: none of its events persist. TL_STATE without
 * a batch, TL_INVALID for NULL, TL_IO for SQL. */
tl_status store_history_rollback(tl_store *store);
/** Stream retained file and desktop opens at or after cutoff, oldest first,
 * each with its retained search's query, for rebuilding the usage summary.
 * One statement, so one consistent read. Rows the store never writes (a
 * negative id or time, an empty desktop id), which only another tool could
 * add, are skipped. Callback errors propagate, plus TL_INVALID for NULL,
 * TL_STATE during a transaction or batch and TL_IO for SQL. The callback
 * must not use this connection. */
tl_status store_history_opens(tl_store *store, int64_t cutoff, tl_store_open_callback callback,
                              void *context);
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
