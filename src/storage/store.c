/* Durable BLOB-path catalog, prepared migration statements and atomic scans. */
#include "torchlight/store.h"
#include <limits.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
struct tl_store {
    sqlite3 *db;
    /* Per-entry statements are prepared once; a scan runs them for every entry. */
    sqlite3_stmt *put, *mark_seen, *keep;
    bool transaction;
};
static const char PUT_SQL[] =
    "INSERT INTO files(path,name,ext,is_dir,mtime,size) VALUES(?1,?2,?3,?4,?5,?6) "
    "ON CONFLICT(path) DO UPDATE SET "
    "name=excluded.name,ext=excluded.ext,is_dir=excluded.is_dir,"
    "mtime=excluded.mtime,size=excluded.size,"
    "emb_version=NULL,emb_bin=NULL,emb_i8=NULL,emb_scale=NULL "
    "WHERE files.mtime IS NOT excluded.mtime OR files.size IS NOT excluded.size "
    "OR files.is_dir IS NOT excluded.is_dir";
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
static tl_status migrate(tl_store *store) {
    int version = 0;
    tl_status status = schema_version(store, &version);
    if (status != TL_OK || version == 1)
        return status;
    if (version != 0)
        return TL_STATE;
    status = execute(store, "BEGIN IMMEDIATE");
    if (status != TL_OK)
        return status;
    /* Another process may have migrated between the read above and BEGIN, so
     * re-read under the write lock before creating anything. */
    status = schema_version(store, &version);
    if (status == TL_OK && version == 0)
        status = create_schema(store);
    else if (status == TL_OK && version != 1)
        status = TL_STATE;
    if (status == TL_OK)
        return execute(store, "COMMIT");
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
        sqlite3_prepare_v2(store->db, KEEP_SQL, -1, &store->keep, NULL) != SQLITE_OK)
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
    if (store->transaction)
        return TL_STATE;
    tl_status status = execute(store, "BEGIN IMMEDIATE");
    if (status != TL_OK)
        return status;
    store->transaction = true;
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
static tl_status one_blob(tl_store *store, const char *sql, const char *path) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(store->db, sql, -1, &statement, NULL) != SQLITE_OK)
        return TL_IO;
    tl_status status = bind_blob(statement, 1, path, strlen(path));
    if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
        status = TL_IO;
    int code = sqlite3_finalize(statement);
    return code == SQLITE_OK ? status : TL_IO;
}
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
    return TL_OK;
}
static tl_status upsert(tl_store *store, const tl_crawl_entry *entry) {
    tl_status status = bind_entry(store->put, entry);
    if (status == TL_OK && sqlite3_step(store->put) != SQLITE_DONE)
        status = TL_IO;
    if (sqlite3_reset(store->put) != SQLITE_OK)
        status = TL_IO;
    sqlite3_clear_bindings(store->put);
    return status;
}
tl_status store_put(tl_store *store, const tl_crawl_entry *entry) {
    if (store == NULL || entry == NULL || entry->path == NULL || entry->path[0] != '/' ||
        entry->size < 0)
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
/* ?1 is the pruned root and ?2 its slash-terminated prefix. Step one keeps
 * other registered roots nested inside it that this scan never visited (e.g. an
 * explicitly indexed hidden directory): they are refreshed by their own scans.
 * Step two deletes unseen entries in scope, except at or below a kept path. */
static const char *const PRUNE_SQL[] = {
    "INSERT OR IGNORE INTO kept SELECT path FROM roots WHERE path<>?1 "
    "AND substr(path,1,length(?2))=?2 AND path NOT IN(SELECT path FROM seen)",
    "DELETE FROM files WHERE (path=?1 OR substr(path,1,length(?2))=?2) "
    "AND path NOT IN(SELECT path FROM seen) AND NOT EXISTS(SELECT 1 FROM kept WHERE "
    "files.path=kept.path OR (substr(files.path,1,length(kept.path))=kept.path "
    "AND substr(files.path,length(kept.path)+1,1)=X'2F'))"};
tl_status store_prune(tl_store *store, const char *root) {
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
    for (size_t i = 0; i < sizeof(PRUNE_SQL) / sizeof(PRUNE_SQL[0]) && status == TL_OK; i++) {
        sqlite3_stmt *statement = NULL;
        if (sqlite3_prepare_v2(store->db, PRUNE_SQL[i], -1, &statement, NULL) != SQLITE_OK)
            status = TL_IO;
        if (status == TL_OK)
            status = bind_blob(statement, 1, root, strlen(root));
        if (status == TL_OK)
            status = bind_blob(statement, 2, prefix, length);
        if (status == TL_OK && sqlite3_step(statement) != SQLITE_DONE)
            status = TL_IO;
        if (sqlite3_finalize(statement) != SQLITE_OK)
            status = TL_IO;
    }
    free(prefix);
    if (status == TL_OK)
        status = one_blob(store, "INSERT OR IGNORE INTO roots VALUES(?1)", root);
    return status;
}
tl_status store_commit(tl_store *store) {
    if (store == NULL)
        return TL_INVALID;
    if (!store->transaction)
        return TL_STATE;
    tl_status status =
        execute(store, "UPDATE meta SET value=CAST(value AS INTEGER)+1 WHERE key='catalog_gen'");
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
    tl_store_entry entry = {(uint64_t)id, path, sqlite3_column_int(statement, 2) != 0};
    tl_status status = callback(context, &entry);
    free(path);
    return status;
}
tl_status store_load(tl_store *store, tl_store_callback callback, void *context) {
    if (store == NULL || callback == NULL)
        return TL_INVALID;
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT id,files.path,EXISTS(SELECT 1 FROM roots WHERE roots.path=files.path) "
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
