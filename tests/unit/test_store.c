/* Stable ids, scope isolation, kept scopes, rollback, raw paths and reopen durability. */
#include "test.h"
#include "torchlight/store.h"
#include <sqlite3.h>
#include <string.h>
#include <unistd.h>
struct loaded {
    size_t count;
    uint64_t first_id, last_id;
    bool opaque;
};
static tl_status observe(void *context, const tl_store_entry *entry) {
    struct loaded *loaded = context;
    if (loaded->count == 0)
        loaded->first_id = entry->id;
    loaded->last_id = entry->id;
    loaded->opaque = loaded->opaque || strchr(entry->path, 0xff) != NULL;
    loaded->count++;
    return TL_OK;
}
static size_t count_entries(tl_store *store) {
    struct loaded loaded = {0};
    CHECK(store_load(store, observe, &loaded) == TL_OK);
    return loaded.count;
}
static void put_path(tl_store *store, const char *path, bool unreadable, bool has_stat) {
    tl_crawl_entry entry = {
        .path = path, .is_dir = true, .unreadable = unreadable, .has_stat = has_stat};
    CHECK(store_put(store, &entry) == TL_OK);
}
/* Regression: re-indexing a parent root deleted an explicitly indexed hidden
 * root inside it, because the parent's crawl skips hidden directories. */
static void nested_root_survives_parent_prune(tl_store *store) {
    CHECK(store_begin(store) == TL_OK);
    put_path(store, "/n/.hidden", false, true);
    put_path(store, "/n/.hidden/secret", false, true);
    CHECK(store_prune(store, "/n/.hidden") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    size_t before = count_entries(store);
    CHECK(store_begin(store) == TL_OK);
    put_path(store, "/n", false, true);
    CHECK(store_prune(store, "/n") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    CHECK(count_entries(store) == before + 1);
}
/* Regression: an unreadable directory failed the whole scan. Its saved subtree
 * and the saved row of a path whose stat failed must survive pruning, while a
 * sibling sharing the byte prefix ("/u/locked2") is still pruned. */
static void unreadable_scopes_are_kept(tl_store *store) {
    CHECK(store_begin(store) == TL_OK);
    const char *paths[] = {"/u", "/u/locked", "/u/locked/child", "/u/locked2", "/u/gone"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        put_path(store, paths[i], false, true);
    CHECK(store_prune(store, "/u") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    size_t before = count_entries(store);
    CHECK(store_begin(store) == TL_OK);
    put_path(store, "/u", false, true);
    put_path(store, "/u/locked", true, true);
    put_path(store, "/u/gone", true, false);
    CHECK(store_prune(store, "/u") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    CHECK(count_entries(store) == before - 1);
}
static tl_status count_root(void *context, const char *root) {
    size_t *count = context;
    CHECK(root[0] == '/');
    (*count)++;
    return TL_OK;
}
/* Forgetting a root drops its unseen entries but spares kept and seen ones. */
static void forgotten_roots(tl_store *store) {
    CHECK(store_begin(store) == TL_OK);
    const char *paths[] = {"/f", "/f/a", "/f/b", "/g", "/g/c"};
    for (size_t i = 0; i < 5; i++)
        put_path(store, paths[i], false, true);
    CHECK(store_prune(store, "/f") == TL_OK && store_prune(store, "/g") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    size_t roots = 0, before = count_entries(store);
    CHECK(store_roots(store, count_root, &roots) == TL_OK);
    CHECK(store_keep(store, "/g") == TL_STATE);
    CHECK(store_begin(store) == TL_OK);
    put_path(store, "/f/a", false, true); /* still seen by some other scan */
    CHECK(store_keep(store, "/g") == TL_OK && store_keep(store, "relative") == TL_INVALID);
    CHECK(store_forget_root(store, "/f") == TL_OK && store_forget_root(store, "/g") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    size_t after_roots = 0;
    CHECK(store_roots(store, count_root, &after_roots) == TL_OK && after_roots == roots - 2);
    CHECK(count_entries(store) == before - 2); /* "/f" and "/f/b" */
}
struct concurrent_load {
    tl_store *writer;
    size_t count;
    bool saw_new;
};
/* Commit on another WAL connection while the reader's snapshot is pinned. */
static tl_status commit_during_load(void *context, const tl_store_entry *entry) {
    struct concurrent_load *load = context;
    if (load->count == 0) {
        CHECK(store_begin(load->writer) == TL_OK);
        put_path(load->writer, "/new-during-load", false, true);
        CHECK(store_commit(load->writer) == TL_OK);
    }
    load->count++;
    load->saw_new |= strcmp(entry->path, "/new-during-load") == 0;
    return TL_OK;
}
static tl_status reject_load(void *context, const tl_store_entry *entry) {
    (void)context;
    (void)entry;
    return TL_LIMIT;
}
static void consistent_catalog_load(tl_store *reader, const char *database) {
    tl_store *writer = NULL;
    CHECK(store_create(database, &writer) == TL_OK);
    struct loaded before = {0};
    uint64_t before_gen = 0, during_gen = 0, after_gen = 0;
    CHECK(store_load_catalog(reader, observe, &before, &before_gen) == TL_OK);
    CHECK(before_gen > 0);
    struct concurrent_load during = {.writer = writer};
    CHECK(store_load_catalog(reader, commit_during_load, &during, &during_gen) == TL_OK);
    CHECK(during_gen == before_gen && during.count == before.count && !during.saw_new);
    struct loaded after = {0};
    CHECK(store_load_catalog(reader, observe, &after, &after_gen) == TL_OK);
    CHECK(after_gen == before_gen + 1 && after.count == before.count + 1);
    CHECK(store_begin(reader) == TL_OK);
    after_gen = 99;
    CHECK(store_load_catalog(reader, observe, &after, &after_gen) == TL_STATE && after_gen == 0);
    CHECK(store_rollback(reader) == TL_OK);
    CHECK(store_load_catalog(reader, reject_load, NULL, &after_gen) == TL_LIMIT && after_gen == 0);
    /* A failed load must close its read transaction before the next write. */
    CHECK(store_begin(reader) == TL_OK && store_rollback(reader) == TL_OK);
    CHECK(store_load_catalog(NULL, observe, &after, &after_gen) == TL_INVALID && after_gen == 0);
    store_destroy(writer);
}
struct ids {
    uint64_t values[16];
    size_t count;
};
static tl_status collect_ids(void *context, const tl_store_entry *entry) {
    struct ids *ids = context;
    CHECK(ids->count < 16);
    ids->values[ids->count++] = entry->id;
    return TL_OK;
}
static uint64_t id_of(tl_store *store, const char *path);
struct lookup {
    const char *path;
    uint64_t id;
};
static tl_status find_path(void *context, const tl_store_entry *entry) {
    struct lookup *lookup = context;
    if (strcmp(entry->path, lookup->path) == 0)
        lookup->id = entry->id;
    return TL_OK;
}
static uint64_t id_of(tl_store *store, const char *path) {
    struct lookup lookup = {path, 0};
    CHECK(store_load(store, find_path, &lookup) == TL_OK);
    return lookup.id;
}
/* Scoped prunes remove only unseen direct children (with subtrees) or unseen
 * descendants, never a byte-prefix sibling, a kept scope or a nested root;
 * the committed change set lists exactly the touched rows. */
static void scoped_prunes_and_changes(void) {
    char database[] = "/tmp/torchlight-scoped-XXXXXX";
    int fd = mkstemp(database);
    CHECK(fd >= 0 && close(fd) == 0);
    tl_store *store = NULL;
    CHECK(store_create(database, &store) == TL_OK);
    CHECK(store_track_changes(store, 16) == TL_OK);
    CHECK(store_begin(store) == TL_OK);
    const char *paths[] = {"/s",   "/s/a",   "/s/a/x",    "/s/b",        "/s/c", "/s/c/y",
                           "/s/k", "/s/k/z", "/s/nested", "/s/nested/n", "/s2"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        put_path(store, paths[i], false, true);
    CHECK(store_prune(store, "/s") == TL_OK && store_prune(store, "/s/nested") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    const uint64_t *changed = NULL;
    size_t count = 0;
    bool complete = false, roots = false;
    CHECK(store_changes(store, &changed, &count, &complete, &roots) == TL_OK);
    CHECK(count == 11 && complete && roots);
    uint64_t b = id_of(store, "/s/b"), c = id_of(store, "/s/c"), y = id_of(store, "/s/c/y");
    uint64_t a = id_of(store, "/s/a"), x = id_of(store, "/s/a/x");
    CHECK(store_prune_children(store, "/s") == TL_STATE); /* outside a transaction */
    /* Children rescan of /s: a seen (its subtree stays), b and c (with y) gone,
     * k unreadable and nested a registered root. */
    CHECK(store_begin(store) == TL_OK);
    put_path(store, "/s", false, true);
    put_path(store, "/s/a", false, true);
    put_path(store, "/s/k", true, true);
    CHECK(store_prune_children(store, "/s") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    CHECK(store_changes(store, &changed, &count, &complete, &roots) == TL_OK);
    CHECK(count == 3 && complete && !roots);
    CHECK(changed[0] == b && changed[1] == c && changed[2] == y);
    CHECK(id_of(store, "/s/a/x") == x && id_of(store, "/s/k/z") != 0 &&
          id_of(store, "/s/nested/n") != 0 && id_of(store, "/s2") != 0);
    /* Tree rescan of /s/a drops the unseen descendant only. */
    CHECK(store_begin(store) == TL_OK);
    put_path(store, "/s/a", false, true);
    CHECK(store_prune_tree(store, "/s/a") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    CHECK(store_changes(store, &changed, &count, &complete, &roots) == TL_OK);
    CHECK(count == 1 && changed[0] == x && id_of(store, "/s/a") == a);
    /* Load by id skips deleted rows and reports the committed generation. */
    uint64_t wanted[] = {a, b, x, id_of(store, "/s2")}, gen = 0;
    struct ids loaded = {0};
    CHECK(store_load_ids(store, wanted, 4, collect_ids, &loaded, &gen) == TL_OK);
    CHECK(loaded.count == 2 && loaded.values[0] == a && gen == 3);
    /* Beyond the limit the change set is incomplete; rollback clears it. */
    CHECK(store_track_changes(store, 1) == TL_OK && store_begin(store) == TL_OK);
    CHECK(store_prune_children(store, "/s") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    CHECK(store_changes(store, &changed, &count, &complete, &roots) == TL_OK && !complete);
    CHECK(store_begin(store) == TL_OK);
    put_path(store, "/s/new", false, true);
    CHECK(store_rollback(store) == TL_OK);
    CHECK(store_changes(store, &changed, &count, &complete, &roots) == TL_OK && count == 0);
    CHECK(store_prune_tree(store, "relative") == TL_INVALID);
    store_destroy(store);
    CHECK(unlink(database) == 0);
}
struct retained {
    size_t count;
    uint64_t file_ids[8];
    char desktop_ids[8][32], queries[8][32];
    int64_t timestamps[8];
};
static tl_status collect_open(void *context, uint64_t file_id, const char *desktop_id,
                              const char *query, int64_t timestamp) {
    struct retained *retained = context;
    CHECK(retained->count < 8);
    size_t i = retained->count++;
    retained->file_ids[i] = file_id;
    snprintf(retained->desktop_ids[i], 32, "%s", desktop_id == NULL ? "" : desktop_id);
    snprintf(retained->queries[i], 32, "%s", query == NULL ? "" : query);
    retained->timestamps[i] = timestamp;
    return TL_OK;
}
static tl_store_history_event open_event(const char *event_id, uint64_t file_id,
                                         const char *search_id, const char *query,
                                         int64_t timestamp) {
    return (tl_store_history_event){.kind = STORE_HISTORY_OPEN,
                                    .event_id = event_id,
                                    .file_id = file_id,
                                    .search_id = search_id,
                                    .query = query,
                                    .timestamp = timestamp};
}
/* Batched history: an open and its search row land together, a rejected
 * event leaves nothing behind, rollback discards the batch, and retained
 * opens stream oldest first with their queries. */
static void history_batches(void) {
    char database[] = "/tmp/torchlight-history-XXXXXX";
    int fd = mkstemp(database);
    CHECK(fd >= 0 && close(fd) == 0);
    tl_store *store = NULL;
    CHECK(store_create(database, &store) == TL_OK);
    CHECK(store_begin(store) == TL_OK);
    put_path(store, "/h", false, true);
    put_path(store, "/h/a", false, true);
    CHECK(store_commit(store) == TL_OK);
    struct loaded rows = {0};
    CHECK(store_load(store, observe, &rows) == TL_OK && rows.count == 2);
    uint64_t a = rows.last_id, h = rows.first_id;
    tl_store_history_event event = open_event("e1", a, "s1", "alp", 10);
    CHECK(store_history_write(store, &event) == TL_OK);
    CHECK(store_history_write(store, &event) == TL_OK); /* idempotent retry */
    CHECK(store_search(store, "s0", NULL, 1) == TL_INVALID);
    event.query = NULL;
    event.search_id = NULL;
    event.event_id = "";
    CHECK(store_history_write(store, &event) == TL_INVALID);
    CHECK(store_history_begin(store) == TL_OK && store_history_begin(store) == TL_STATE);
    CHECK(store_begin(store) == TL_STATE && store_history_prune(store, 0, true) == TL_STATE);
    tl_store_history_event desktop = {.kind = STORE_HISTORY_DESKTOP_OPEN,
                                      .event_id = "e2",
                                      .desktop_id = "firefox.desktop",
                                      .search_id = "s2",
                                      .query = "fire",
                                      .timestamp = 5};
    CHECK(store_history_write(store, &desktop) == TL_OK);
    event = open_event("e3", 999, "s-missing", "gone", 15); /* no such file */
    CHECK(store_history_write(store, &event) == TL_STATE);
    event = open_event("e4", h, NULL, NULL, 20);
    CHECK(store_history_write(store, &event) == TL_OK);
    CHECK(store_history_commit(store) == TL_OK && store_history_commit(store) == TL_STATE);
    CHECK(store_history_begin(store) == TL_OK);
    event = open_event("e5", h, "s5", "discarded", 30);
    CHECK(store_history_write(store, &event) == TL_OK);
    CHECK(store_history_rollback(store) == TL_OK);
    /* The rejected open's search row was rolled back with it. */
    event = open_event("e6", h, "s-missing", NULL, 25);
    CHECK(store_history_write(store, &event) == TL_OK);
    struct retained retained = {0};
    CHECK(store_history_opens(store, 0, collect_open, &retained) == TL_OK && retained.count == 4);
    CHECK(retained.file_ids[0] == 0 && strcmp(retained.desktop_ids[0], "firefox.desktop") == 0 &&
          strcmp(retained.queries[0], "fire") == 0 && retained.timestamps[0] == 5);
    CHECK(retained.file_ids[1] == a && strcmp(retained.queries[1], "alp") == 0);
    CHECK(retained.file_ids[2] == h && retained.queries[2][0] == 0);
    CHECK(retained.file_ids[3] == h && retained.queries[3][0] == 0 && retained.timestamps[3] == 25);
    retained = (struct retained){0};
    CHECK(store_history_opens(store, 6, collect_open, &retained) == TL_OK && retained.count == 3);
    CHECK(store_history_opens(store, 0, NULL, NULL) == TL_INVALID);
    CHECK(store_history_prune(store, 0, true) == TL_OK);
    retained = (struct retained){0};
    CHECK(store_history_opens(store, 0, collect_open, &retained) == TL_OK && retained.count == 0);
    store_destroy(store);
    CHECK(unlink(database) == 0);
}
/* Run SQL on a separate connection to database: fault injection, and rows
 * that the store's own writes would reject (only another tool writes them). */
static void raw_sql(const char *database, const char *sql) {
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(database, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_close(db) == SQLITE_OK);
}
static int64_t raw_count(const char *database, const char *sql) {
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    CHECK(sqlite3_open(database, &db) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, sql, -1, &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    int64_t count = sqlite3_column_int64(statement, 0);
    CHECK(sqlite3_finalize(statement) == SQLITE_OK && sqlite3_close(db) == SQLITE_OK);
    return count;
}
/* When SQLite rolls back a whole history batch (an I/O error, out of memory
 * or, injected here, a trigger), the batch ends with it: the failing write
 * reports TL_IO, commit finds no batch, and a new batch can begin.
 * Regression: the batch stayed open in name, later writes committed on their
 * own, and the commit failed with TL_IO although they were saved. */
static void history_batch_lost(void) {
    char database[] = "/tmp/torchlight-lost-XXXXXX";
    int fd = mkstemp(database);
    CHECK(fd >= 0 && close(fd) == 0);
    tl_store *store = NULL;
    CHECK(store_create(database, &store) == TL_OK);
    raw_sql(database, "CREATE TRIGGER lose_batch BEFORE INSERT ON searches "
                      "WHEN NEW.query = 'boom' BEGIN SELECT RAISE(ROLLBACK, 'injected'); END");
    CHECK(store_history_begin(store) == TL_OK);
    CHECK(store_search(store, "s1", "lost with the batch", 1) == TL_OK);
    CHECK(store_search(store, "s2", "boom", 2) == TL_IO);
    CHECK(store_history_commit(store) == TL_STATE);
    CHECK(store_history_begin(store) == TL_OK);
    CHECK(store_search(store, "s3", "next batch", 3) == TL_OK);
    CHECK(store_history_commit(store) == TL_OK);
    CHECK(raw_count(database, "SELECT count(*) FROM searches") == 1);
    CHECK(raw_count(database, "SELECT count(*) FROM searches WHERE id = 's3'") == 1);
    store_destroy(store);
    CHECK(unlink(database) == 0);
}
/* Retained rows the store would never write are skipped while rebuilding
 * usage, and the rest are still reported. Regression: one such row failed
 * the whole read, so personal ranking started empty on every restart. */
static void history_malformed_rows(void) {
    char database[] = "/tmp/torchlight-malformed-XXXXXX";
    int fd = mkstemp(database);
    CHECK(fd >= 0 && close(fd) == 0);
    tl_store *store = NULL;
    CHECK(store_create(database, &store) == TL_OK);
    raw_sql(database, "INSERT INTO desktop_opens VALUES('negative-time','a.desktop',NULL,-5),"
                      "('kept','b.desktop',NULL,100),('empty-id','',NULL,101)");
    struct retained retained = {0};
    CHECK(store_history_opens(store, -10, collect_open, &retained) == TL_OK);
    CHECK(retained.count == 1 && strcmp(retained.desktop_ids[0], "b.desktop") == 0 &&
          retained.timestamps[0] == 100);
    store_destroy(store);
    CHECK(unlink(database) == 0);
}
void test_store(void) {
    scoped_prunes_and_changes();
    history_batches();
    history_batch_lost();
    history_malformed_rows();
    char database[] = "/tmp/torchlight-store-XXXXXX";
    int fd = mkstemp(database);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
    tl_store *store = NULL;
    CHECK(store_create(database, &store) == TL_OK);
    CHECK(store_embedding_activate(store, 7) == TL_STATE);
    CHECK(store_embedding_stage(store, 7, "[model,tokenizer,preprocessing,projection,int8]") ==
          TL_OK);
    CHECK(store_embedding_stage(store, 7, "conflicting model") == TL_STATE);
    float embedding[] = {0.6F, 0.8F}, cached[2];
    const float invalid_embedding[] = {2, 2};
    CHECK(store_embedding_put(store, 7, "bad", invalid_embedding, 2) == TL_INVALID);
    bool found = true;
    CHECK(store_embedding_get(store, 7, "invoice pdf", cached, 2, &found) == TL_OK && !found);
    CHECK(store_embedding_put(store, 7, "invoice pdf", embedding, 2) == TL_OK);
    CHECK(store_embedding_activate(store, 7) == TL_OK);
    CHECK(store_embedding_get(store, 7, "invoice pdf", cached, 2, &found) == TL_OK && found);
    CHECK(cached[0] == embedding[0] && cached[1] == embedding[1]);
    CHECK(store_embedding_get(store, 8, "invoice pdf", cached, 2, &found) == TL_OK && !found);
    CHECK(store_embedding_stage(store, 8, "new model") == TL_OK);
    CHECK(store_embedding_activate(store, 8) == TL_OK);
    CHECK(store_embedding_get(store, 7, "invoice pdf", cached, 2, &found) == TL_OK && !found);
    uint64_t empty_gen = 99;
    struct loaded empty = {0};
    CHECK(store_load_catalog(store, observe, &empty, &empty_gen) == TL_OK);
    CHECK(empty_gen == 0 && empty.count == 0);
    tl_crawl_entry a = {.path = "/root/a\xff", .has_stat = true, .size = 1},
                   b = {.path = "/root2/b", .has_stat = true, .size = 2};
    CHECK(store_put(store, &a) == TL_STATE);
    CHECK(store_begin(store) == TL_OK);
    CHECK(store_put(store, &a) == TL_OK);
    CHECK(store_put(store, &b) == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    struct loaded original = {0};
    CHECK(store_load(store, observe, &original) == TL_OK);
    CHECK(original.count == 2 && original.opaque);
    CHECK(store_begin(store) == TL_OK);
    a.size = 3;
    CHECK(store_put(store, &a) == TL_OK);
    CHECK(store_prune(store, "/root") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    struct loaded after = {0};
    CHECK(store_load(store, observe, &after) == TL_OK);
    CHECK(after.count == 2 && after.first_id == original.first_id);
    CHECK(store_begin(store) == TL_OK);
    CHECK(store_prune(store, "/root") == TL_OK);
    CHECK(store_rollback(store) == TL_OK);
    after = (struct loaded){0};
    CHECK(store_load(store, observe, &after) == TL_OK && after.count == 2);
    CHECK(store_begin(store) == TL_OK);
    CHECK(store_prune(store, "/root") == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    CHECK(store_begin(store) == TL_OK);
    CHECK(store_put(store, &a) == TL_OK);
    CHECK(store_commit(store) == TL_OK);
    after = (struct loaded){0};
    CHECK(store_load(store, observe, &after) == TL_OK);
    CHECK(after.last_id > original.last_id);
    nested_root_survives_parent_prune(store);
    unreadable_scopes_are_kept(store);
    forgotten_roots(store);
    store_destroy(store);
    CHECK(store_create(database, &store) == TL_OK);
    after = (struct loaded){0};
    CHECK(store_load(store, observe, &after) == TL_OK && after.count == 12);
    consistent_catalog_load(store, database);
    store_destroy(store);
    CHECK(unlink(database) == 0);
}
