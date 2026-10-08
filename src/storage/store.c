/* Durable BLOB-path catalog, prepared migration statements and atomic scans. */
#include "torchlight/store.h"
#include "torchlight/json.h"
#include "torchlight/path.h"
#include <limits.h>
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define STORE_SCHEMA_VERSION 4
#define IDENTITY_BYTES 29
#define IDENTITY_OBJECT_BYTES 16
#define IDENTITY_RENAMED 2
#define NANOSECONDS_PER_SECOND 1000000000U
/* History statements, prepared on first use and reused for every event. */
enum { HISTORY_SEARCH, HISTORY_OPEN, HISTORY_DESKTOP_OPEN, HISTORY_STATEMENTS };
static const char *const HISTORY_SQL[HISTORY_STATEMENTS] = {
    /* Idempotent per id; a different query for an existing id changes nothing. */
    "INSERT INTO searches VALUES(?1,?2,?3) ON CONFLICT(id) DO UPDATE SET "
    "id=excluded.id WHERE searches.query=excluded.query",
    "INSERT INTO opens(event_id,file_id,search_id,ts) SELECT ?1,?2,(SELECT id "
    "FROM searches WHERE id=?3),?4 WHERE EXISTS(SELECT 1 FROM files WHERE id=?2) "
    "ON CONFLICT(event_id) DO UPDATE SET event_id=excluded.event_id WHERE "
    "opens.file_id=excluded.file_id AND opens.search_id IS excluded.search_id",
    "INSERT INTO desktop_opens VALUES(?1,?2,(SELECT id FROM searches WHERE "
    "id=?3),?4) ON CONFLICT(event_id) DO UPDATE SET event_id=excluded.event_id "
    "WHERE desktop_opens.desktop_id=excluded.desktop_id AND "
    "desktop_opens.search_id IS excluded.search_id"};
struct tl_store {
    sqlite3 *db;
    /* Per-entry statements are prepared once; a scan runs them for every entry. */
    sqlite3_stmt *put, *mark_seen, *keep, *identity;
    sqlite3_stmt *embedding_get, *embedding_put, *embedding_touch;
    sqlite3_stmt *history[HISTORY_STATEMENTS];
    /* history_batch: a store_history_begin transaction is open. */
    bool transaction, reading, changed, embedding_batch, history_batch;
    /* Ids of files rows inserted, updated or deleted by the current catalog
     * transaction (see store_changes), up to change_limit before overflow. */
    uint64_t *change_ids;
    size_t change_count, change_capacity, change_limit;
    bool changes_overflow, roots_changed;
};
/* SQLite reports every row write on this connection, including deletions by
 * scoped DELETE statements and the explicit destination delete of a move, so
 * the recorded ids are exactly the rows a committed batch touched. */
static void record_change(tl_store *store, sqlite3_int64 row) {
    if (store->changes_overflow || row <= 0)
        return;
    if (store->change_count == store->change_limit) {
        store->changes_overflow = true;
        return;
    }
    if (store->change_count == store->change_capacity) {
        size_t capacity = store->change_capacity == 0 ? 64 : store->change_capacity * 2;
        uint64_t *grown = realloc(store->change_ids, capacity * sizeof(uint64_t));
        if (grown == NULL) {
            store->changes_overflow = true;
            return;
        }
        store->change_ids = grown;
        store->change_capacity = capacity;
    }
    store->change_ids[store->change_count++] = (uint64_t)row;
}
static void catalog_change(void *context, int operation, const char *database, const char *table,
                           sqlite3_int64 row) {
    tl_store *store = context;
    (void)operation;
    if (strcmp(database, "main") != 0)
        return;
    if (strcmp(table, "files") == 0) {
        store->changed = true;
        record_change(store, row);
    } else if (strcmp(table, "roots") == 0) {
        store->changed = true;
        store->roots_changed = true;
    }
}
static const char PUT_SQL[] =
    "INSERT INTO files(path,name,ext,is_dir,mtime,size,identity) VALUES(?1,?2,?3,?4,?5,?6,?7) "
    "ON CONFLICT(path) DO UPDATE SET "
    "name=excluded.name,ext=excluded.ext,is_dir=excluded.is_dir,"
    "mtime=excluded.mtime,size=excluded.size,identity=COALESCE(excluded.identity,files.identity),"
    "emb_version=NULL,emb_bin=NULL,emb_i8=NULL,emb_scale=NULL "
    "WHERE files.mtime IS NOT excluded.mtime OR files.size IS NOT excluded.size "
    "OR files.is_dir IS NOT excluded.is_dir "
    "OR (excluded.identity IS NOT NULL AND files.identity IS NOT excluded.identity)";
static const char MARK_SEEN_SQL[] = "INSERT OR IGNORE INTO seen VALUES(?1)";
static const char KEEP_SQL[] = "INSERT OR IGNORE INTO kept VALUES(?1)";
static tl_status execute(tl_store *store, const char *sql) {
    sqlite3_stmt *statement = NULL;
    int code = sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL);
    if (code != SQLITE_OK)
        return TL_IO;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
    }
    int finalized = sqlite3_finalize(statement);
    return code == SQLITE_DONE && finalized == SQLITE_OK ? TL_OK : TL_IO;
}
static tl_status schema_version(tl_store *store, int *out) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, "PRAGMA user_version", -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    int code = sqlite3_step(statement);
    if (code == SQLITE_ROW)
        *out = sqlite3_column_int(statement, 0);
    int finalized = sqlite3_finalize(statement);
    return code == SQLITE_ROW && finalized == SQLITE_OK ? TL_OK : TL_IO;
}
static tl_status create_schema(tl_store *store) {
    const char *steps[] = {
        "CREATE TABLE files(id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "path BLOB UNIQUE NOT NULL CHECK(typeof(path)='blob'),"
        "name BLOB NOT NULL CHECK(typeof(name)='blob'),"
        "ext BLOB CHECK(ext IS NULL OR typeof(ext)='blob'),"
        "is_dir INTEGER NOT NULL CHECK(is_dir IN(0,1)),mtime INTEGER,size INTEGER,"
        "emb_version TEXT,emb_bin BLOB,emb_i8 BLOB,emb_scale REAL)",
        "CREATE TABLE searches(id TEXT NOT NULL PRIMARY KEY,query TEXT NOT NULL,ts INTEGER NOT "
        "NULL)",
        "CREATE TABLE opens(event_id TEXT NOT NULL PRIMARY KEY,"
        "file_id INTEGER NOT NULL REFERENCES files(id) ON DELETE CASCADE,"
        "search_id TEXT REFERENCES searches(id) ON DELETE SET NULL,ts INTEGER NOT NULL)",
        "CREATE INDEX opens_file_ts ON opens(file_id,ts)",
        "CREATE INDEX opens_search ON opens(search_id)",
        "CREATE TABLE meta(key TEXT NOT NULL PRIMARY KEY,value TEXT)",
        "INSERT INTO meta VALUES('schema_version','1'),('catalog_gen','0')",
        "CREATE TABLE roots(path BLOB PRIMARY KEY NOT NULL CHECK(typeof(path)='blob'))",
        "PRAGMA user_version=1"};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        tl_status status = execute(store, steps[i]);
        if (status != TL_OK)
            return status;
    }
    return TL_OK;
}
static tl_status migrate_identity(tl_store *store) {
    const char *steps[] = {
        "ALTER TABLE files ADD COLUMN identity BLOB "
        "CHECK(identity IS NULL OR (typeof(identity)='blob' AND length(identity)=29))",
        "UPDATE meta SET value='2' WHERE key='schema_version'", "PRAGMA user_version=2"};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        tl_status status = execute(store, steps[i]);
        if (status != TL_OK)
            return status;
    }
    return TL_OK;
}
static tl_status migrate_desktop_history(tl_store *store) {
    const char *steps[] = {
        "CREATE TABLE desktop_opens(event_id TEXT NOT NULL PRIMARY KEY,"
        "desktop_id TEXT NOT NULL,search_id TEXT REFERENCES searches(id) ON DELETE SET NULL,"
        "ts INTEGER NOT NULL)",
        "CREATE INDEX desktop_opens_ts ON desktop_opens(ts)",
        "UPDATE meta SET value='3' WHERE key='schema_version'", "PRAGMA user_version=3"};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        tl_status status = execute(store, steps[i]);
        if (status != TL_OK)
            return status;
    }
    return TL_OK;
}
static tl_status migrate(tl_store *store) {
    int version = 0;
    tl_status status = schema_version(store, &version);
    if (status != TL_OK || version == STORE_SCHEMA_VERSION)
        return status;
    if (version < 0 || version > STORE_SCHEMA_VERSION)
        return TL_STATE;
    status = execute(store, "BEGIN IMMEDIATE");
    if (status != TL_OK)
        return status;
    /* Another process may have migrated between the read above and BEGIN, so
     * re-read under the write lock before creating anything. */
    status = schema_version(store, &version);
    if (status == TL_OK && version == 0) {
        status = create_schema(store);
        version = 1;
    }
    if (status == TL_OK && version == 1) {
        status = migrate_identity(store);
        version = 2;
    }
    if (status == TL_OK && version == 2) {
        status = migrate_desktop_history(store);
        version = 3;
    }
    if (status == TL_OK && version == 3) {
        const char *steps[] = {"CREATE TABLE embedding_models(emb_gen TEXT PRIMARY KEY,descriptor "
                               "TEXT NOT NULL,active INTEGER NOT NULL DEFAULT 0)",
                               "CREATE TABLE embedding_cache(emb_gen TEXT NOT NULL REFERENCES "
                               "embedding_models(emb_gen) ON DELETE CASCADE,"
                               "text BLOB NOT NULL,payload BLOB NOT NULL,touched INTEGER NOT NULL "
                               "DEFAULT 1,PRIMARY KEY(emb_gen,text)) WITHOUT ROWID",
                               "UPDATE meta SET value='4' WHERE key='schema_version'",
                               "PRAGMA user_version=4"};
        for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]) && status == TL_OK; i++)
            status = execute(store, steps[i]);
    }
    if (status == TL_OK)
        status = execute(store, "COMMIT");
    if (status == TL_OK)
        return TL_OK;
    tl_status rollback = execute(store, "ROLLBACK");
    return rollback == TL_OK ? status : rollback;
}
static tl_status enable_wal(tl_store *store) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, "PRAGMA journal_mode=WAL", -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    /* SQLite reports the resulting mode; it silently keeps another mode when the
     * filesystem or VFS cannot provide WAL. */
    tl_status status = TL_IO;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char *mode = sqlite3_column_text(statement, 0);
        if (mode != NULL && strcmp((const char *)mode, "wal") == 0)
            status = TL_OK;
    }
    return sqlite3_finalize(statement) == SQLITE_OK ? status : TL_IO;
}
static tl_status prepare_statements(tl_store *store) {
    if (sqlite3_prepare_v2(store->db, PUT_SQL, -1, &store->put, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(store->db, MARK_SEEN_SQL, -1, &store->mark_seen, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(store->db, KEEP_SQL, -1, &store->keep, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(store->db, "SELECT identity FROM files WHERE path=?1", -1,
                           &store->identity, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(store->db,
                           "SELECT payload FROM embedding_cache WHERE emb_gen=?1 AND text=?2", -1,
                           &store->embedding_get, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(
            store->db,
            "INSERT INTO embedding_cache(emb_gen,text,payload,touched) VALUES(?1,?2,?3,1) "
            "ON CONFLICT(emb_gen,text) DO UPDATE SET payload=excluded.payload,touched=1",
            -1, &store->embedding_put, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(store->db,
                           "UPDATE embedding_cache SET touched=1 WHERE emb_gen=?1 AND text=?2", -1,
                           &store->embedding_touch, NULL) != SQLITE_OK)
        return TL_IO;
    return TL_OK;
}
tl_status store_create(const char *path, tl_store **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (path == NULL || path[0] == 0)
        return TL_INVALID;
    tl_store *store = calloc(1, sizeof(*store));
    if (store == NULL)
        return TL_NOMEM;
    int code = sqlite3_open_v2(path, &store->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    tl_status status = code == SQLITE_OK ? TL_OK : TL_IO;
    if (status == TL_OK && sqlite3_busy_timeout(store->db, 3000) != SQLITE_OK)
        status = TL_IO;
    if (status == TL_OK)
        status = execute(store, "PRAGMA foreign_keys=ON");
    if (status == TL_OK)
        status = enable_wal(store);
    if (status == TL_OK)
        status = migrate(store);
    if (status == TL_OK)
        status = execute(store, "CREATE TEMP TABLE seen(path BLOB PRIMARY KEY)");
    if (status == TL_OK)
        status = execute(store, "CREATE TEMP TABLE kept(path BLOB PRIMARY KEY)");
    if (status == TL_OK)
        status = prepare_statements(store);
    if (status == TL_OK)
        sqlite3_update_hook(store->db, catalog_change, store);
    if (status != TL_OK) {
        store_destroy(store);
        return status;
    }
    *out = store;
    return TL_OK;
}
void store_destroy(tl_store *store) {
    if (store == NULL)
        return;
    sqlite3_finalize(store->put);
    sqlite3_finalize(store->mark_seen);
    sqlite3_finalize(store->keep);
    sqlite3_finalize(store->identity);
    sqlite3_finalize(store->embedding_get);
    sqlite3_finalize(store->embedding_put);
    sqlite3_finalize(store->embedding_touch);
    for (size_t i = 0; i < HISTORY_STATEMENTS; i++)
        sqlite3_finalize(store->history[i]);
    free(store->change_ids);
    if (store->db != NULL) {
        /* Closing the connection rolls back any unfinished transaction. */
        int code = sqlite3_close_v2(store->db);
        (void)code;
    }
    free(store);
}
tl_status store_begin(tl_store *store) {
    if (store == NULL)
        return TL_INVALID;
    if (store->transaction || store->reading || store->embedding_batch || store->history_batch)
        return TL_STATE;
    tl_status status = execute(store, "BEGIN IMMEDIATE");
    if (status != TL_OK)
        return status;
    store->transaction = true;
    store->changed = false;
    store->change_count = 0;
    store->changes_overflow = false;
    store->roots_changed = false;
    status = execute(store, "DELETE FROM seen");
    if (status == TL_OK)
        status = execute(store, "DELETE FROM kept");
    return status;
}
static tl_status bind_blob(sqlite3_stmt *statement, int parameter, const char *bytes,
                           size_t length) {
    if (length > INT_MAX)
        return TL_LIMIT;
    return sqlite3_bind_blob(statement, parameter, bytes, (int)length, SQLITE_TRANSIENT) ==
                   SQLITE_OK
               ? TL_OK
               : TL_IO;
}
/* Run a cached single-BLOB statement, leaving it reset and unbound for reuse. */
static tl_status run_cached(sqlite3_stmt *statement, const char *path) {
    tl_status status = bind_blob(statement, 1, path, strlen(path));
    if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
        status = TL_IO;
    if (sqlite3_reset(statement) != SQLITE_OK)
        status = TL_IO;
    sqlite3_clear_bindings(statement);
    return status;
}
/* Canonical little-endian encoding is independent of C struct padding/ABI.
 * Kind 0 is ctime, 1 is birth time, 2 retains only a paired rename's object key. */
static void identity_number(unsigned char *out, size_t bytes, uint64_t value) {
    for (size_t i = 0; i < bytes; i++)
        out[i] = (unsigned char)(value >> (i * 8));
}
static void encode_identity(const tl_crawl_entry *entry, unsigned char out[IDENTITY_BYTES]) {
    out[0] = entry->identity_birth ? 1 : 0;
    identity_number(out + 1, 8, entry->device);
    identity_number(out + 9, 8, entry->inode);
    identity_number(out + 17, 8, (uint64_t)entry->identity_sec);
    identity_number(out + 25, 4, entry->identity_nsec);
}
static tl_status identity_changed(tl_store *store, const tl_crawl_entry *entry, bool *changed) {
    *changed = false;
    if (!entry->has_identity)
        return TL_OK;
    unsigned char incoming[IDENTITY_BYTES];
    encode_identity(entry, incoming);
    sqlite3_stmt *statement = store->identity;
    tl_status status = bind_blob(statement, 1, entry->path, strlen(entry->path));
    if (status == TL_OK) {
        int code = sqlite3_step(statement);
        if (code == SQLITE_ROW && sqlite3_column_type(statement, 0) != SQLITE_NULL) {
            const unsigned char *saved = sqlite3_column_blob(statement, 0);
            if (saved == NULL || sqlite3_column_type(statement, 0) != SQLITE_BLOB ||
                sqlite3_column_bytes(statement, 0) != IDENTITY_BYTES || saved[0] > IDENTITY_RENAMED)
                status = TL_IO;
            else
                *changed =
                    memcmp(saved + 1, incoming + 1, IDENTITY_OBJECT_BYTES) != 0 ||
                    (saved[0] != IDENTITY_RENAMED && memcmp(saved, incoming, IDENTITY_BYTES) != 0);
        } else if (code != SQLITE_ROW && code != SQLITE_DONE)
            status = TL_IO;
    }
    if (sqlite3_reset(statement) != SQLITE_OK)
        status = TL_IO;
    sqlite3_clear_bindings(statement);
    return status;
}
static tl_status retire_scope(tl_store *store, const char *path);
static tl_status bind_entry(sqlite3_stmt *statement, const tl_crawl_entry *entry) {
    const char *name = strrchr(entry->path, '/');
    name = name == NULL ? entry->path : name + 1;
    const char *extension = entry->is_dir ? NULL : strrchr(name, '.');
    if (extension == name || (extension != NULL && extension[1] == 0))
        extension = NULL;
    tl_status status = bind_blob(statement, 1, entry->path, strlen(entry->path));
    if (status == TL_OK)
        status = bind_blob(statement, 2, name, strlen(name));
    if (status == TL_OK && extension != NULL)
        status = bind_blob(statement, 3, extension + 1, strlen(extension + 1));
    if (status != TL_OK)
        return status;
    if (sqlite3_bind_int(statement, 4, entry->is_dir ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int64(statement, 5, entry->mtime) != SQLITE_OK ||
        sqlite3_bind_int64(statement, 6, entry->size) != SQLITE_OK)
        return TL_IO;
    if (entry->has_identity) {
        unsigned char identity[IDENTITY_BYTES];
        encode_identity(entry, identity);
        if (sqlite3_bind_blob(statement, 7, identity, IDENTITY_BYTES, SQLITE_TRANSIENT) !=
            SQLITE_OK)
            return TL_IO;
    }
    return TL_OK;
}
static tl_status upsert(tl_store *store, const tl_crawl_entry *entry) {
    bool replaced = false;
    tl_status status = identity_changed(store, entry, &replaced);
    if (status == TL_OK && replaced)
        status = retire_scope(store, entry->path);
    if (status == TL_OK)
        status = bind_entry(store->put, entry);
    if (status == TL_OK && sqlite3_step(store->put) != SQLITE_DONE)
        status = TL_IO;
    if (sqlite3_reset(store->put) != SQLITE_OK)
        status = TL_IO;
    sqlite3_clear_bindings(store->put);
    return status;
}
tl_status store_put(tl_store *store, const tl_crawl_entry *entry) {
    if (store == NULL || entry == NULL || entry->path == NULL || entry->path[0] != '/' ||
        entry->size < 0 ||
        (entry->has_identity &&
         (!entry->has_stat || entry->identity_nsec >= NANOSECONDS_PER_SECOND)))
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    tl_status status = TL_OK;
    /* Without stat data the saved row (if any) is the best information we have. */
    if (entry->has_stat)
        status = upsert(store, entry);
    if (status == TL_OK)
        status = run_cached(store->mark_seen, entry->path);
    if (status == TL_OK && entry->unreadable)
        status = run_cached(store->keep, entry->path);
    return status;
}
/* Statements below bind ?1 to a root and ?2 to its slash-terminated prefix.
 * Deleting unseen entries in scope spares everything at or below a kept path. */
#define DELETE_UNSEEN_IN_SCOPE                                                                     \
    "DELETE FROM files WHERE (path=?1 OR substr(path,1,length(?2))=?2) "                           \
    "AND path NOT IN(SELECT path FROM seen) AND NOT EXISTS(SELECT 1 FROM kept WHERE "              \
    "files.path=kept.path OR (substr(files.path,1,length(kept.path))=kept.path "                   \
    "AND substr(files.path,length(kept.path)+1,1)=X'2F'))"
/* Pruning first keeps other registered roots nested inside it that this scan
 * never visited (e.g. an explicitly indexed hidden directory): they are
 * refreshed by their own scans. */
static const char *const PRUNE_SQL[] = {
    "INSERT OR IGNORE INTO kept SELECT path FROM roots WHERE path<>?1 "
    "AND substr(path,1,length(?2))=?2 AND path NOT IN(SELECT path FROM seen)",
    DELETE_UNSEEN_IN_SCOPE, "INSERT OR IGNORE INTO roots VALUES(?1)"};
/* Forgetting unregisters the root, then drops what no scan in this
 * transaction saw or kept. */
static const char *const FORGET_SQL[] = {"DELETE FROM roots WHERE path=?1", DELETE_UNSEEN_IN_SCOPE};
static tl_status run_statement(tl_store *store, const char *sql, const char *root,
                               const char *prefix) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    int parameters = sqlite3_bind_parameter_count(statement);
    tl_status status = bind_blob(statement, 1, root, strlen(root));
    if (status == TL_OK && parameters >= 2)
        status = bind_blob(statement, 2, prefix, strlen(prefix));
    if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
        status = TL_IO;
    if (sqlite3_finalize(statement) != SQLITE_OK)
        status = TL_IO;
    return status;
}
static tl_status run_scoped(tl_store *store, const char *const *statements, size_t count,
                            const char *root) {
    if (store == NULL || root == NULL || root[0] != '/')
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    size_t length = strlen(root);
    if (length > INT_MAX - 1)
        return TL_LIMIT;
    char *prefix = malloc(length + 2);
    if (prefix == NULL)
        return TL_NOMEM;
    memcpy(prefix, root, length);
    if (length != 1)
        prefix[length++] = '/';
    prefix[length] = 0;
    tl_status status = TL_OK;
    for (size_t i = 0; i < count && status == TL_OK; i++)
        status = run_statement(store, statements[i], root, prefix);
    free(prefix);
    return status;
}
static tl_status retire_scope(tl_store *store, const char *path) {
    const char *steps[] = {"DELETE FROM files WHERE path=?1 OR substr(path,1,length(?2))=?2",
                           "DELETE FROM seen WHERE path=?1 OR substr(path,1,length(?2))=?2",
                           "DELETE FROM kept WHERE path=?1 OR substr(path,1,length(?2))=?2"};
    return run_scoped(store, steps, sizeof(steps) / sizeof(steps[0]), path);
}
tl_status store_prune(tl_store *store, const char *root) {
    return run_scoped(store, PRUNE_SQL, sizeof(PRUNE_SQL) / sizeof(PRUNE_SQL[0]), root);
}
tl_status store_forget_root(tl_store *store, const char *root) {
    return run_scoped(store, FORGET_SQL, sizeof(FORGET_SQL) / sizeof(FORGET_SQL[0]), root);
}
tl_status store_keep(tl_store *store, const char *path) {
    if (store == NULL || path == NULL || path[0] != '/')
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    return run_cached(store->keep, path);
}
/* Scoped prunes bind ?1 to the directory, ?2 to its slash-terminated prefix
 * and ?3 to the first byte string after every path with that prefix, so the
 * path index answers the range instead of a scan of the whole catalog. Kept
 * scopes and registered roots nested in the directory spare their subtrees. */
#define SPARE_KEPT_AND_NESTED_ROOTS                                                                \
    " AND NOT EXISTS(SELECT 1 FROM kept WHERE files.path=kept.path OR "                            \
    "(substr(files.path,1,length(kept.path))=kept.path AND "                                       \
    "substr(files.path,length(kept.path)+1,1)=X'2F'))"                                             \
    " AND NOT EXISTS(SELECT 1 FROM roots WHERE roots.path>=?2 AND roots.path<?3 AND "              \
    "(files.path=roots.path OR (substr(files.path,1,length(roots.path))=roots.path AND "           \
    "substr(files.path,length(roots.path)+1,1)=X'2F')))"
static const char PRUNE_TREE_SQL[] =
    "DELETE FROM files WHERE path>=?2 AND path<?3 AND "
    "path NOT IN(SELECT path FROM seen)" SPARE_KEPT_AND_NESTED_ROOTS;
/* A row survives a children rescan when its direct-child component was seen. */
static const char PRUNE_CHILDREN_SQL[] =
    "DELETE FROM files WHERE path>=?2 AND path<?3 AND "
    "(CASE WHEN instr(substr(path,length(?2)+1),X'2F')=0 THEN path "
    "ELSE substr(path,1,length(?2)+instr(substr(path,length(?2)+1),X'2F')-1) END) "
    "NOT IN(SELECT path FROM seen)" SPARE_KEPT_AND_NESTED_ROOTS;
static tl_status prune_range(tl_store *store, const char *sql, const char *directory) {
    if (store == NULL || directory == NULL || directory[0] != '/')
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    size_t length = strlen(directory);
    if (length > INT_MAX - 2)
        return TL_LIMIT;
    char *prefix = malloc(length + 2), *limit = malloc(length + 2);
    tl_status status = prefix == NULL || limit == NULL ? TL_NOMEM : TL_OK;
    if (status == TL_OK) {
        memcpy(prefix, directory, length);
        if (length != 1)
            prefix[length++] = '/';
        memcpy(limit, prefix, length);
        limit[length - 1] = (char)('/' + 1); /* "dir/" < every "dir/..." < "dir0" */
    }
    sqlite3_stmt *statement = NULL;
    if (status == TL_OK && sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL) != SQLITE_OK)
        status = TL_IO;
    if (status == TL_OK)
        status = bind_blob(statement, 1, directory, strlen(directory));
    if (status == TL_OK)
        status = bind_blob(statement, 2, prefix, length);
    if (status == TL_OK)
        status = bind_blob(statement, 3, limit, length);
    if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
        status = TL_IO;
    if (statement != NULL && sqlite3_finalize(statement) != SQLITE_OK)
        status = TL_IO;
    free(prefix);
    free(limit);
    return status;
}
tl_status store_prune_children(tl_store *store, const char *directory) {
    return prune_range(store, PRUNE_CHILDREN_SQL, directory);
}
tl_status store_prune_tree(tl_store *store, const char *directory) {
    return prune_range(store, PRUNE_TREE_SQL, directory);
}
tl_status store_track_changes(tl_store *store, size_t limit) {
    if (store == NULL)
        return TL_INVALID;
    if (store->transaction)
        return TL_STATE;
    store->change_limit = limit;
    store->change_count = 0;
    store->changes_overflow = false;
    return TL_OK;
}
static int compare_ids(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return a < b ? -1 : a > b;
}
tl_status store_changes(tl_store *store, const uint64_t **ids, size_t *count, bool *complete,
                        bool *roots_changed) {
    if (store == NULL || ids == NULL || count == NULL || complete == NULL || roots_changed == NULL)
        return TL_INVALID;
    if (store->transaction)
        return TL_STATE;
    if (store->change_count > 1)
        qsort(store->change_ids, store->change_count, sizeof(uint64_t), compare_ids);
    size_t unique = 0;
    for (size_t i = 0; i < store->change_count; i++)
        if (unique == 0 || store->change_ids[unique - 1] != store->change_ids[i])
            store->change_ids[unique++] = store->change_ids[i];
    store->change_count = unique;
    *ids = store->change_ids;
    *count = unique;
    *complete = !store->changes_overflow && store->change_limit != 0;
    *roots_changed = store->roots_changed;
    return TL_OK;
}
/* Validate the whole decimal value: SQLite's CAST silently accepts malformed
 * text and saturates at INT64_MAX, which could reuse a catalog_gen. */
static tl_status read_catalog_gen(tl_store *store, uint64_t *out) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, "SELECT value FROM meta WHERE key='catalog_gen'", -1,
                           &statement, NULL) != SQLITE_OK)
        return TL_IO;
    tl_status status = TL_IO;
    if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) == SQLITE_TEXT) {
        const unsigned char *value = sqlite3_column_text(statement, 0);
        int length = sqlite3_column_bytes(statement, 0);
        uint64_t number = 0;
        status = value != NULL && length > 0 ? TL_OK : TL_IO;
        for (int i = 0; i < length && status == TL_OK; i++) {
            if (value[i] < '0' || value[i] > '9' ||
                number > ((uint64_t)INT64_MAX - (uint64_t)(value[i] - '0')) / 10) {
                status = TL_IO;
                break;
            }
            number = number * 10 + (uint64_t)(value[i] - '0');
        }
        if (status == TL_OK)
            *out = number;
    }
    return sqlite3_finalize(statement) == SQLITE_OK ? status : TL_IO;
}
static tl_status advance_catalog_gen(tl_store *store) {
    uint64_t catalog_gen = 0;
    tl_status status = read_catalog_gen(store, &catalog_gen);
    if (status != TL_OK)
        return status;
    if (catalog_gen == INT64_MAX)
        return TL_LIMIT;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, "UPDATE meta SET value=?1 WHERE key='catalog_gen'", -1,
                           &statement, NULL) != SQLITE_OK)
        return TL_IO;
    if (sqlite3_bind_int64(statement, 1, (sqlite3_int64)(catalog_gen + 1)) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_DONE || sqlite3_changes(store->db) != 1)
        status = TL_IO;
    return sqlite3_finalize(statement) == SQLITE_OK ? status : TL_IO;
}
tl_status store_commit(tl_store *store) {
    if (store == NULL)
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    tl_status status = advance_catalog_gen(store);
    if (status == TL_OK)
        status = execute(store, "COMMIT");
    if (status == TL_OK)
        store->transaction = false;
    return status;
}
tl_status store_rollback(tl_store *store) {
    if (store == NULL)
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    tl_status status = execute(store, "ROLLBACK");
    if (status == TL_OK)
        store->transaction = false;
    /* Rolled-back writes never reached a committed view. */
    store->change_count = 0;
    store->changes_overflow = false;
    store->roots_changed = false;
    return status;
}
static tl_status load_row(sqlite3_stmt *statement, tl_store_callback callback, void *context) {
    sqlite3_int64 id = sqlite3_column_int64(statement, 0);
    int length = sqlite3_column_bytes(statement, 1);
    const void *bytes = sqlite3_column_blob(statement, 1);
    if (id <= 0 || length <= 0 || bytes == NULL ||
        sqlite3_column_type(statement, 1) != SQLITE_BLOB ||
        memchr(bytes, 0, (size_t)length) != NULL)
        return TL_IO;
    char *path = malloc((size_t)length + 1);
    if (path == NULL)
        return TL_NOMEM;
    memcpy(path, bytes, (size_t)length);
    path[length] = 0;
    tl_store_entry entry = {(uint64_t)id, path, sqlite3_column_int(statement, 2) != 0,
                            sqlite3_column_int(statement, 3) != 0};
    tl_status status = callback(context, &entry);
    free(path);
    return status;
}
tl_status store_load(tl_store *store, tl_store_callback callback, void *context) {
    if (store == NULL || callback == NULL)
        return TL_INVALID;
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT id,files.path,EXISTS(SELECT 1 FROM roots WHERE roots.path=files.path),files.is_dir "
        "FROM files ORDER BY id";
    if (sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    tl_status status = TL_OK;
    int code = SQLITE_DONE;
    while ((code = sqlite3_step(statement)) == SQLITE_ROW) {
        status = load_row(statement, callback, context);
        if (status != TL_OK)
            break;
    }
    if (status == TL_OK && code != SQLITE_DONE)
        status = TL_IO;
    int finalized = sqlite3_finalize(statement);
    return finalized == SQLITE_OK ? status : TL_IO;
}
tl_status store_load_catalog(tl_store *store, tl_store_callback callback, void *context,
                             uint64_t *out_catalog_gen) {
    if (out_catalog_gen == NULL)
        return TL_INVALID;
    *out_catalog_gen = 0;
    if (store == NULL || callback == NULL)
        return TL_INVALID;
    if (store->transaction || store->reading)
        return TL_STATE;
    tl_status status = execute(store, "BEGIN");
    if (status != TL_OK)
        return status;
    store->reading = true;
    uint64_t catalog_gen = 0;
    status = read_catalog_gen(store, &catalog_gen);
    if (status == TL_OK)
        status = store_load(store, callback, context);
    if (status == TL_OK)
        status = execute(store, "COMMIT");
    if (status != TL_OK) {
        tl_status rollback = execute(store, "ROLLBACK");
        if (rollback != TL_OK)
            status = rollback;
    }
    store->reading = false;
    if (status == TL_OK)
        *out_catalog_gen = catalog_gen;
    return status;
}
/* Report the committed row of every id that still exists, in the given order. */
static tl_status load_ids(tl_store *store, const uint64_t *ids, size_t count,
                          tl_store_callback callback, void *context) {
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT id,files.path,EXISTS(SELECT 1 FROM roots WHERE roots.path=files.path),files.is_dir "
        "FROM files WHERE id=?1";
    if (sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    tl_status status = TL_OK;
    for (size_t i = 0; i < count && status == TL_OK; i++) {
        if (ids[i] > INT64_MAX ||
            sqlite3_bind_int64(statement, 1, (sqlite3_int64)ids[i]) != SQLITE_OK) {
            status = TL_INVALID;
            break;
        }
        int code = sqlite3_step(statement);
        if (code == SQLITE_ROW)
            status = load_row(statement, callback, context);
        else if (code != SQLITE_DONE)
            status = TL_IO;
        if (sqlite3_reset(statement) != SQLITE_OK && status == TL_OK)
            status = TL_IO;
    }
    return sqlite3_finalize(statement) == SQLITE_OK ? status : TL_IO;
}
tl_status store_load_ids(tl_store *store, const uint64_t *ids, size_t count,
                         tl_store_callback callback, void *context, uint64_t *out_catalog_gen) {
    if (out_catalog_gen == NULL)
        return TL_INVALID;
    *out_catalog_gen = 0;
    if (store == NULL || callback == NULL || (ids == NULL && count != 0))
        return TL_INVALID;
    if (store->transaction || store->reading)
        return TL_STATE;
    tl_status status = execute(store, "BEGIN");
    if (status != TL_OK)
        return status;
    store->reading = true;
    uint64_t catalog_gen = 0;
    status = read_catalog_gen(store, &catalog_gen);
    if (status == TL_OK)
        status = load_ids(store, ids, count, callback, context);
    if (status == TL_OK)
        status = execute(store, "COMMIT");
    if (status != TL_OK) {
        tl_status rollback = execute(store, "ROLLBACK");
        if (rollback != TL_OK)
            status = rollback;
    }
    store->reading = false;
    if (status == TL_OK)
        *out_catalog_gen = catalog_gen;
    return status;
}
tl_status store_roots(tl_store *store, tl_store_root_callback callback, void *context) {
    if (store == NULL || callback == NULL)
        return TL_INVALID;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, "SELECT path FROM roots ORDER BY path", -1, &statement,
                           NULL) != SQLITE_OK)
        return TL_IO;
    tl_status status = TL_OK;
    int code = SQLITE_DONE;
    while (status == TL_OK && (code = sqlite3_step(statement)) == SQLITE_ROW) {
        int length = sqlite3_column_bytes(statement, 0);
        const void *bytes = sqlite3_column_blob(statement, 0);
        if (length <= 0 || bytes == NULL || memchr(bytes, 0, (size_t)length) != NULL) {
            status = TL_IO; /* corrupt row: roots are nonempty, NUL-free paths */
            break;
        }
        char *path = malloc((size_t)length + 1);
        if (path == NULL) {
            status = TL_NOMEM;
            break;
        }
        memcpy(path, bytes, (size_t)length);
        path[length] = 0;
        status = callback(context, path);
        free(path);
    }
    if (status == TL_OK && code != SQLITE_DONE)
        status = TL_IO;
    return sqlite3_finalize(statement) == SQLITE_OK ? status : TL_IO;
}
tl_status store_prepare_catalog(tl_store *store, tl_store_callback callback, void *context,
                                uint64_t *out_catalog_gen) {
    if (out_catalog_gen == NULL)
        return TL_INVALID;
    *out_catalog_gen = 0;
    if (store == NULL || callback == NULL)
        return TL_INVALID;
    if (!store->transaction || store->reading)
        return TL_STATE;
    uint64_t catalog_gen = 0;
    tl_status status = read_catalog_gen(store, &catalog_gen);
    if (status == TL_OK && catalog_gen == INT64_MAX)
        status = TL_LIMIT;
    if (status == TL_OK)
        status = store_load(store, callback, context);
    if (status == TL_OK)
        *out_catalog_gen = catalog_gen + 1;
    return status;
}
tl_status store_catalog_changed(tl_store *store, bool *out) {
    if (store == NULL || out == NULL)
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    *out = store->changed;
    return TL_OK;
}
static tl_status move_statement(tl_store *store, const char *sql, const char *old_path,
                                const char *new_path) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    tl_status status = bind_blob(statement, 1, old_path, strlen(old_path));
    if (status == TL_OK)
        status = bind_blob(statement, 2, new_path, strlen(new_path));
    const char *name = strrchr(new_path, '/') + 1, *ext = strrchr(name, '.');
    if (status == TL_OK && sqlite3_bind_parameter_count(statement) >= 3)
        status = bind_blob(statement, 3, name, strlen(name));
    if (status == TL_OK && sqlite3_bind_parameter_count(statement) >= 4 && ext != NULL &&
        ext != name && ext[1] != 0)
        status = bind_blob(statement, 4, ext + 1, strlen(ext + 1));
    if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
        status = TL_IO;
    return sqlite3_finalize(statement) == SQLITE_OK ? status : TL_IO;
}
tl_status store_move(tl_store *store, const char *old_path, const char *new_path) {
    if (store == NULL || old_path == NULL || new_path == NULL || old_path[0] != '/' ||
        new_path[0] != '/' || path_within(old_path, new_path) || path_within(new_path, old_path))
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    /* SQLite concatenation produces TEXT even for BLOB operands; cast before
     * writing so byte paths never acquire a second TEXT identity. */
    static const char DELETE_DEST[] =
        "DELETE FROM files WHERE (path=?2 OR (substr(path,1,length(?2))=?2 AND "
        "substr(path,length(?2)+1,1)=X'2F')) AND EXISTS(SELECT 1 FROM files WHERE path=?1)";
    static const char MOVE[] =
        "UPDATE files SET path=CAST(?2 || substr(path,length(?1)+1) AS BLOB),"
        "name=CASE WHEN path=?1 THEN ?3 ELSE name END,"
        "ext=CASE WHEN path=?1 THEN CASE WHEN is_dir=1 THEN NULL ELSE ?4 END ELSE ext END,"
        "emb_version=NULL,emb_bin=NULL,emb_i8=NULL,emb_scale=NULL,"
        /* Rename changes ctime. Kind 2 retains device/inode so the following
         * scan can adopt its new fallback stamp without losing paired ids. */
        "identity=CASE WHEN substr(identity,1,1)=X'00' "
        "THEN CAST(X'02' || substr(identity,2,16) || zeroblob(12) AS BLOB) ELSE identity END "
        "WHERE path=?1 OR (substr(path,1,length(?1))=?1 AND substr(path,length(?1)+1,1)=X'2F')";
    tl_status status = move_statement(store, DELETE_DEST, old_path, new_path);
    if (status == TL_OK)
        status = move_statement(store, MOVE, old_path, new_path);
    if (status == TL_OK && sqlite3_changes(store->db) != 0) {
        const char *temporary[] = {
            "UPDATE OR REPLACE seen SET path=CAST(?2 || substr(path,length(?1)+1) AS BLOB) WHERE "
            "path=?1 OR (substr(path,1,length(?1))=?1 AND substr(path,length(?1)+1,1)=X'2F')",
            "UPDATE OR REPLACE kept SET path=CAST(?2 || substr(path,length(?1)+1) AS BLOB) WHERE "
            "path=?1 OR (substr(path,1,length(?1))=?1 AND substr(path,length(?1)+1,1)=X'2F')"};
        for (size_t i = 0; i < 2 && status == TL_OK; i++)
            status = move_statement(store, temporary[i], old_path, new_path);
    }
    return status;
}
static tl_status history_ready(const tl_store *store, const char *id) {
    if (store == NULL || id == NULL || id[0] == 0 || !json_utf8(id))
        return TL_INVALID;
    if (strlen(id) > 128)
        return TL_LIMIT;
    return store->transaction || store->reading ? TL_STATE : TL_OK;
}
static bool optional_id(const char *id) {
    return id == NULL || (json_utf8(id) && strlen(id) <= 128);
}
/* Validate one event's fields for its kind before any SQL runs. */
static tl_status history_valid(const tl_store *store, const tl_store_history_event *event) {
    if (event == NULL)
        return TL_INVALID;
    bool search = event->kind == STORE_HISTORY_SEARCH;
    tl_status status = history_ready(store, search ? event->search_id : event->event_id);
    if (status != TL_OK)
        return status;
    if (event->timestamp < 0 || !optional_id(event->search_id) ||
        (event->query != NULL && (event->search_id == NULL || !json_utf8(event->query))))
        return TL_INVALID;
    if (event->query != NULL && strlen(event->query) > 256)
        return TL_LIMIT;
    if (search)
        return event->query == NULL ? TL_INVALID : TL_OK;
    if (event->kind == STORE_HISTORY_OPEN)
        return event->file_id == 0 || event->file_id > INT64_MAX ? TL_INVALID : TL_OK;
    if (event->kind != STORE_HISTORY_DESKTOP_OPEN || event->desktop_id == NULL ||
        event->desktop_id[0] == 0 || !json_utf8(event->desktop_id))
        return TL_INVALID;
    return strlen(event->desktop_id) >= 4096 ? TL_INVALID : TL_OK;
}
/* Prepare a history statement once, for the connection's lifetime. */
static tl_status history_statement(tl_store *store, size_t which, sqlite3_stmt **out) {
    if (store->history[which] == NULL &&
        sqlite3_prepare_v3(store->db, HISTORY_SQL[which], -1, SQLITE_PREPARE_PERSISTENT,
                           &store->history[which], NULL) != SQLITE_OK)
        return TL_IO;
    *out = store->history[which];
    return TL_OK;
}
/* Step a bound history statement that must change exactly one row (TL_STATE
 * for a conflicting retry or missing file), then reset it for reuse. */
static tl_status run_history(tl_store *store, sqlite3_stmt *statement, bool bound) {
    tl_status status = TL_OK;
    if (!bound || sqlite3_step(statement) != SQLITE_DONE)
        status = TL_IO;
    else if (sqlite3_changes(store->db) != 1)
        status = TL_STATE;
    if (sqlite3_reset(statement) != SQLITE_OK && status == TL_OK)
        status = TL_IO;
    sqlite3_clear_bindings(statement);
    return status;
}
static bool bind_text(sqlite3_stmt *statement, int parameter, const char *text) {
    return text == NULL
               ? sqlite3_bind_null(statement, parameter) == SQLITE_OK
               : sqlite3_bind_text(statement, parameter, text, -1, SQLITE_TRANSIENT) == SQLITE_OK;
}
static tl_status insert_search(tl_store *store, const tl_store_history_event *event) {
    sqlite3_stmt *statement = NULL;
    tl_status status = history_statement(store, HISTORY_SEARCH, &statement);
    if (status != TL_OK)
        return status;
    bool bound = bind_text(statement, 1, event->search_id) &&
                 bind_text(statement, 2, event->query) &&
                 sqlite3_bind_int64(statement, 3, event->timestamp) == SQLITE_OK;
    return run_history(store, statement, bound);
}
static tl_status insert_open(tl_store *store, const tl_store_history_event *event) {
    bool desktop = event->kind == STORE_HISTORY_DESKTOP_OPEN;
    sqlite3_stmt *statement = NULL;
    tl_status status =
        history_statement(store, desktop ? HISTORY_DESKTOP_OPEN : HISTORY_OPEN, &statement);
    if (status != TL_OK)
        return status;
    bool bound =
        bind_text(statement, 1, event->event_id) &&
        (desktop ? bind_text(statement, 2, event->desktop_id)
                 : sqlite3_bind_int64(statement, 2, (sqlite3_int64)event->file_id) == SQLITE_OK) &&
        bind_text(statement, 3, event->search_id) &&
        sqlite3_bind_int64(statement, 4, event->timestamp) == SQLITE_OK;
    return run_history(store, statement, bound);
}
tl_status store_history_write(tl_store *store, const tl_store_history_event *event) {
    tl_status status = history_valid(store, event);
    if (status != TL_OK)
        return status;
    /* A savepoint makes an open and its search row one unit, inside a batch
     * or as its own transaction. */
    status = execute(store, "SAVEPOINT history_event");
    if (status != TL_OK)
        return status;
    if (event->kind == STORE_HISTORY_SEARCH || event->query != NULL)
        status = insert_search(store, event);
    if (status == TL_OK && event->kind != STORE_HISTORY_SEARCH)
        status = insert_open(store, event);
    if (status != TL_OK && execute(store, "ROLLBACK TO history_event") != TL_OK)
        status = TL_IO;
    tl_status released = execute(store, "RELEASE history_event");
    return status == TL_OK ? released : status;
}
tl_status store_history_begin(tl_store *store) {
    if (store == NULL)
        return TL_INVALID;
    if (store->transaction || store->reading || store->embedding_batch || store->history_batch)
        return TL_STATE;
    tl_status status = execute(store, "BEGIN IMMEDIATE");
    store->history_batch = status == TL_OK;
    return status;
}
static tl_status end_history(tl_store *store, const char *sql) {
    if (store == NULL)
        return TL_INVALID;
    if (!store->history_batch)
        return TL_STATE;
    tl_status status = execute(store, sql);
    /* A failed COMMIT leaves the transaction open; roll it back so the
     * connection is usable again and the batch is reported as lost. */
    if (status != TL_OK && sqlite3_get_autocommit(store->db) == 0 &&
        execute(store, "ROLLBACK") != TL_OK)
        status = TL_IO;
    store->history_batch = false;
    return status;
}
tl_status store_history_commit(tl_store *store) {
    return end_history(store, "COMMIT");
}
tl_status store_history_rollback(tl_store *store) {
    return end_history(store, "ROLLBACK");
}
tl_status store_search(tl_store *store, const char *id, const char *query, int64_t timestamp) {
    tl_store_history_event event = {
        .kind = STORE_HISTORY_SEARCH, .search_id = id, .query = query, .timestamp = timestamp};
    return store_history_write(store, &event);
}
tl_status store_open_event(tl_store *store, const char *event_id, uint64_t file_id,
                           const char *search_id, int64_t timestamp) {
    tl_store_history_event event = {.kind = STORE_HISTORY_OPEN,
                                    .search_id = search_id,
                                    .event_id = event_id,
                                    .file_id = file_id,
                                    .timestamp = timestamp};
    return store_history_write(store, &event);
}
tl_status store_desktop_open(tl_store *store, const char *event_id, const char *desktop_id,
                             const char *search_id, int64_t timestamp) {
    tl_store_history_event event = {.kind = STORE_HISTORY_DESKTOP_OPEN,
                                    .search_id = search_id,
                                    .event_id = event_id,
                                    .desktop_id = desktop_id,
                                    .timestamp = timestamp};
    return store_history_write(store, &event);
}
/* Opens of both kinds with their retained queries, oldest first. */
static const char *const HISTORY_OPENS_SQL =
    "SELECT o.file_id, NULL, s.query, o.ts FROM opens o LEFT JOIN searches s "
    "ON s.id = o.search_id WHERE o.ts >= ?1 UNION ALL "
    "SELECT 0, d.desktop_id, s.query, d.ts FROM desktop_opens d LEFT JOIN searches s "
    "ON s.id = d.search_id WHERE d.ts >= ?1 ORDER BY 4";
static tl_status report_open(sqlite3_stmt *statement, tl_store_open_callback callback,
                             void *context) {
    sqlite3_int64 file_id = sqlite3_column_int64(statement, 0);
    const char *desktop_id = (const char *)sqlite3_column_text(statement, 1);
    const char *query = (const char *)sqlite3_column_text(statement, 2);
    sqlite3_int64 timestamp = sqlite3_column_int64(statement, 3);
    if (file_id < 0 || timestamp < 0 || (file_id == 0) == (desktop_id == NULL))
        return TL_IO;
    return callback(context, (uint64_t)file_id, desktop_id, query, (int64_t)timestamp);
}
tl_status store_history_opens(tl_store *store, int64_t cutoff, tl_store_open_callback callback,
                              void *context) {
    if (store == NULL || callback == NULL)
        return TL_INVALID;
    if (store->transaction || store->reading || store->history_batch)
        return TL_STATE;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, HISTORY_OPENS_SQL, -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    tl_status status = sqlite3_bind_int64(statement, 1, cutoff) == SQLITE_OK ? TL_OK : TL_IO;
    int code = SQLITE_ROW;
    while (status == TL_OK && (code = sqlite3_step(statement)) == SQLITE_ROW)
        status = report_open(statement, callback, context);
    if (status == TL_OK && code != SQLITE_DONE)
        status = TL_IO;
    return sqlite3_finalize(statement) == SQLITE_OK || status != TL_OK ? status : TL_IO;
}
tl_status store_history_prune(tl_store *store, int64_t cutoff, bool clear) {
    if (store == NULL || cutoff < 0)
        return TL_INVALID;
    if (store->transaction || store->reading || store->history_batch)
        return TL_STATE;
    const char *steps[] = {"DELETE FROM opens WHERE ?1 OR ts<?2",
                           "DELETE FROM desktop_opens WHERE ?1 OR ts<?2",
                           "DELETE FROM searches WHERE ?1 OR ts<?2"};
    tl_status status = execute(store, "BEGIN IMMEDIATE");
    if (status != TL_OK)
        return status;
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]) && status == TL_OK; i++) {
        sqlite3_stmt *statement = NULL;
        if (sqlite3_prepare_v2(store->db, steps[i], -1, &statement, NULL) != SQLITE_OK) {
            status = TL_IO;
            break;
        }
        if (sqlite3_bind_int(statement, 1, clear ? 1 : 0) != SQLITE_OK ||
            sqlite3_bind_int64(statement, 2, cutoff) != SQLITE_OK ||
            sqlite3_step(statement) != SQLITE_DONE)
            status = TL_IO;
        if (sqlite3_finalize(statement) != SQLITE_OK)
            status = TL_IO;
    }
    if (status == TL_OK)
        status = execute(store, "COMMIT");
    if (status != TL_OK) {
        tl_status rollback = execute(store, "ROLLBACK");
        if (rollback != TL_OK)
            status = rollback;
    }
    return status;
}

static tl_status embedding_bind(sqlite3_stmt *statement, uint64_t emb_gen, const char *text) {
    char generation[21];
    int length = snprintf(generation, sizeof(generation), "%llu", (unsigned long long)emb_gen);
    if (length < 0 || (size_t)length >= sizeof(generation))
        return TL_LIMIT;
    if (sqlite3_bind_text(statement, 1, generation, -1, SQLITE_TRANSIENT) != SQLITE_OK)
        return TL_IO;
    return text == NULL ? TL_OK : bind_blob(statement, 2, text, strlen(text));
}
static tl_status embedding_execute(tl_store *store, const char *sql, uint64_t emb_gen,
                                   const char *descriptor) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    tl_status status = embedding_bind(statement, emb_gen, NULL);
    if (status == TL_OK && descriptor != NULL &&
        sqlite3_bind_text(statement, 2, descriptor, -1, SQLITE_TRANSIENT) != SQLITE_OK)
        status = TL_IO;
    if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
        status = TL_IO;
    if (sqlite3_finalize(statement) != SQLITE_OK)
        status = TL_IO;
    return status;
}
tl_status store_embedding_stage(tl_store *store, uint64_t emb_gen, const char *descriptor) {
    if (store == NULL || emb_gen == 0 || descriptor == NULL || !json_utf8(descriptor) ||
        strlen(descriptor) > 4096 || descriptor[0] == 0)
        return TL_INVALID;
    if (store->transaction || store->reading || store->embedding_batch)
        return TL_STATE;
    tl_status status =
        embedding_execute(store,
                          "INSERT INTO embedding_models(emb_gen,descriptor) VALUES(?1,?2) "
                          "ON CONFLICT(emb_gen) DO UPDATE SET descriptor=excluded.descriptor "
                          "WHERE descriptor=excluded.descriptor",
                          emb_gen, descriptor);
    if (status == TL_OK && sqlite3_changes(store->db) == 0)
        return TL_STATE;
    if (status == TL_OK)
        status = embedding_execute(store, "UPDATE embedding_cache SET touched=0 WHERE emb_gen=?1",
                                   emb_gen, NULL);
    return status;
}
static bool embedding_decode(sqlite3_stmt *statement, float *values, size_t dimensions) {
    const unsigned char *bytes = sqlite3_column_blob(statement, 0);
    bool valid = bytes != NULL;
    double norm = 0;
    for (size_t i = 0; i < dimensions && valid; i++) {
        uint32_t bits = (uint32_t)bytes[i * 4] | (uint32_t)bytes[i * 4 + 1] << 8 |
                        (uint32_t)bytes[i * 4 + 2] << 16 | (uint32_t)bytes[i * 4 + 3] << 24;
        memcpy(values + i, &bits, sizeof(float));
        valid = isfinite(values[i]);
        norm += (double)values[i] * values[i];
    }
    return valid && fabs(norm - 1) < 1e-4;
}
tl_status store_embedding_get(tl_store *store, uint64_t emb_gen, const char *text, float *values,
                              size_t dimensions, bool *out_found) {
    if (out_found != NULL)
        *out_found = false;
    if (store == NULL || emb_gen == 0 || text == NULL || values == NULL || out_found == NULL ||
        dimensions == 0 || dimensions > 4096)
        return TL_INVALID;
    if (store->transaction || store->reading)
        return TL_STATE;
    sqlite3_stmt *statement = store->embedding_get;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    tl_status status = embedding_bind(statement, emb_gen, text);
    int code = status == TL_OK ? sqlite3_step(statement) : SQLITE_ERROR;
    if (code == SQLITE_ROW && sqlite3_column_bytes(statement, 0) == (int)(dimensions * 4)) {
        *out_found = embedding_decode(statement, values, dimensions);
    } else if (code != SQLITE_ROW && code != SQLITE_DONE) {
        status = TL_IO;
    }
    if (sqlite3_reset(statement) != SQLITE_OK)
        status = TL_IO;
    if (status == TL_OK && *out_found) {
        statement = store->embedding_touch;
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        status = embedding_bind(statement, emb_gen, text);
        if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
            status = TL_IO;
        if (sqlite3_reset(statement) != SQLITE_OK)
            status = TL_IO;
    }
    return status;
}
tl_status store_embedding_put(tl_store *store, uint64_t emb_gen, const char *text,
                              const float *values, size_t dimensions) {
    if (store == NULL || emb_gen == 0 || text == NULL || values == NULL || dimensions == 0 ||
        dimensions > 4096)
        return TL_INVALID;
    if (store->transaction || store->reading)
        return TL_STATE;
    unsigned char payload[4096 * 4];
    double norm = 0;
    for (size_t i = 0; i < dimensions; i++) {
        if (!isfinite(values[i]))
            return TL_INVALID;
        norm += (double)values[i] * values[i];
        uint32_t bits = 0;
        memcpy(&bits, values + i, sizeof(float));
        for (size_t j = 0; j < 4; j++)
            payload[i * 4 + j] = (unsigned char)(bits >> (j * 8));
    }
    if (fabs(norm - 1) >= 1e-4)
        return TL_INVALID;
    sqlite3_stmt *statement = store->embedding_put;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    tl_status status = embedding_bind(statement, emb_gen, text);
    if (status == TL_OK)
        status = bind_blob(statement, 3, (const char *)payload, dimensions * 4);
    if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
        status = TL_IO;
    if (sqlite3_reset(statement) != SQLITE_OK)
        status = TL_IO;
    return status;
}
tl_status store_embedding_activate(tl_store *store, uint64_t emb_gen) {
    if (store == NULL || emb_gen == 0)
        return TL_INVALID;
    if (store->transaction || store->reading || store->embedding_batch)
        return TL_STATE;
    tl_status status = execute(store, "BEGIN IMMEDIATE");
    if (status != TL_OK)
        return status;
    status = embedding_execute(store, "UPDATE embedding_models SET active=1 WHERE emb_gen=?1",
                               emb_gen, NULL);
    if (status == TL_OK && sqlite3_changes(store->db) == 0)
        status = TL_STATE;
    const char *steps[] = {"UPDATE embedding_models SET active=(emb_gen=?1)",
                           "DELETE FROM embedding_cache WHERE emb_gen!=?1 OR touched=0",
                           "DELETE FROM embedding_models WHERE emb_gen!=?1"};
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]) && status == TL_OK; i++)
        status = embedding_execute(store, steps[i], emb_gen, NULL);
    if (status == TL_OK)
        status = execute(store, "COMMIT");
    if (status != TL_OK) {
        tl_status rollback = execute(store, "ROLLBACK");
        if (rollback != TL_OK)
            return rollback;
    }
    return status;
}

tl_status store_embedding_batch_begin(tl_store *store) {
    if (store == NULL)
        return TL_INVALID;
    if (store->transaction || store->reading || store->embedding_batch)
        return TL_STATE;
    tl_status status = execute(store, "BEGIN IMMEDIATE");
    if (status == TL_OK)
        store->embedding_batch = true;
    return status;
}
tl_status store_embedding_batch_end(tl_store *store, bool commit) {
    if (store == NULL)
        return TL_INVALID;
    if (!store->embedding_batch)
        return TL_STATE;
    tl_status status = execute(store, commit ? "COMMIT" : "ROLLBACK");
    if (status != TL_OK && commit) {
        tl_status rollback = execute(store, "ROLLBACK");
        if (rollback != TL_OK)
            status = rollback;
    }
    store->embedding_batch = false;
    return status;
}
