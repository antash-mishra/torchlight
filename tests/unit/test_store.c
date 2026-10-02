/* Stable ids, scope isolation, kept scopes, rollback, raw paths and reopen durability. */
#include "test.h"
#include "torchlight/store.h"
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
void test_store(void) {
    char database[] = "/tmp/torchlight-store-XXXXXX";
    int fd = mkstemp(database);
    CHECK(fd >= 0);
    CHECK(close(fd) == 0);
    tl_store *store = NULL;
    CHECK(store_create(database, &store) == TL_OK);
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
    store_destroy(store);
    CHECK(unlink(database) == 0);
}
