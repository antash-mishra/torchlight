/* Filesystem incarnation changes retire ids atomically, including reused inodes. */
#include "test.h"
#include "torchlight/store.h"
#include <string.h>
#include <unistd.h>

struct find_entry {
    const char *path;
    uint64_t id;
};
static tl_status find_path(void *context, const tl_store_entry *entry) {
    struct find_entry *found = context;
    if (strcmp(found->path, entry->path) == 0)
        found->id = entry->id;
    return TL_OK;
}
static uint64_t saved_id(tl_store *store, const char *path) {
    struct find_entry found = {.path = path};
    CHECK(store_load(store, find_path, &found) == TL_OK);
    return found.id;
}
static void save_entry(tl_store *store, const tl_crawl_entry *entry) {
    CHECK(store_begin(store) == TL_OK && store_put(store, entry) == TL_OK);
    CHECK(store_commit(store) == TL_OK);
}
static void replacement_and_rollback(tl_store *store) {
    tl_crawl_entry entry = {.path = "/root/file\xff",
                            .has_stat = true,
                            .has_identity = true,
                            .identity_birth = true,
                            .device = UINT64_MAX,
                            .inode = UINT64_MAX,
                            .identity_sec = 10,
                            .identity_nsec = 5};
    save_entry(store, &entry);
    uint64_t original = saved_id(store, entry.path);
    CHECK(original != 0 && store_open_event(store, "original-open", original, NULL, 1) == TL_OK);
    entry.size = 10;
    entry.mtime = 20;
    save_entry(store, &entry);
    CHECK(saved_id(store, entry.path) == original);
    /* A same-inode, same-metadata replacement must still get a fresh id. */
    entry.identity_nsec++;
    CHECK(store_begin(store) == TL_OK && store_put(store, &entry) == TL_OK);
    CHECK(saved_id(store, entry.path) > original);
    CHECK(store_rollback(store) == TL_OK && saved_id(store, entry.path) == original);
    CHECK(store_open_event(store, "original-open", original, NULL, 1) == TL_OK);
    save_entry(store, &entry);
    uint64_t replacement = saved_id(store, entry.path);
    CHECK(replacement > original);
    CHECK(store_open_event(store, "stale-open", original, NULL, 1) == TL_STATE);
    entry.inode--;
    save_entry(store, &entry);
    CHECK(saved_id(store, entry.path) > replacement);
    entry.identity_nsec = 1000000000U;
    CHECK(store_begin(store) == TL_OK && store_put(store, &entry) == TL_INVALID);
    CHECK(store_rollback(store) == TL_OK);
}
static void directory_replacement(tl_store *store) {
    tl_crawl_entry folder = {.path = "/tree/dir",
                             .is_dir = true,
                             .has_stat = true,
                             .has_identity = true,
                             .identity_birth = true,
                             .device = 1,
                             .inode = 2,
                             .identity_sec = 10};
    tl_crawl_entry child = folder;
    child.path = "/tree/dir/child";
    child.inode = 3;
    child.is_dir = false;
    tl_crawl_entry sibling = child;
    sibling.path = "/tree/dir2";
    sibling.inode = 4;
    save_entry(store, &folder);
    save_entry(store, &child);
    save_entry(store, &sibling);
    uint64_t folder_id = saved_id(store, folder.path), child_id = saved_id(store, child.path);
    uint64_t sibling_id = saved_id(store, sibling.path);
    folder.identity_sec++;
    CHECK(store_begin(store) == TL_OK && store_keep(store, folder.path) == TL_OK);
    CHECK(store_put(store, &folder) == TL_OK && saved_id(store, child.path) == 0);
    CHECK(store_put(store, &child) == TL_OK && store_commit(store) == TL_OK);
    CHECK(saved_id(store, folder.path) > folder_id && saved_id(store, child.path) > child_id);
    CHECK(saved_id(store, sibling.path) == sibling_id);
}
static void fallback_and_legacy(tl_store *store) {
    tl_crawl_entry entry = {.path = "/fallback/old", .has_stat = true};
    save_entry(store, &entry);
    uint64_t original = saved_id(store, entry.path);
    entry.has_identity = true;
    entry.device = 1;
    entry.inode = 10;
    entry.identity_sec = 1;
    save_entry(store, &entry);
    CHECK(saved_id(store, entry.path) == original);
    entry.identity_sec++;
    save_entry(store, &entry);
    uint64_t fallback = saved_id(store, entry.path);
    CHECK(fallback > original);
    CHECK(store_begin(store) == TL_OK);
    CHECK(store_move(store, entry.path, "/fallback/new") == TL_OK);
    entry.path = "/fallback/new";
    entry.identity_sec++;
    CHECK(store_put(store, &entry) == TL_OK && store_commit(store) == TL_OK);
    CHECK(saved_id(store, entry.path) == fallback);
    /* A rename must not hide a later replacement's different object key. */
    CHECK(store_begin(store) == TL_OK);
    CHECK(store_move(store, entry.path, "/fallback/replaced") == TL_OK);
    entry.path = "/fallback/replaced";
    entry.inode++;
    CHECK(store_put(store, &entry) == TL_OK && store_commit(store) == TL_OK);
    CHECK(saved_id(store, entry.path) > fallback);
}
void test_identity(void) {
    char database[] = "/tmp/torchlight-identity-XXXXXX";
    int fd = mkstemp(database);
    CHECK(fd >= 0 && close(fd) == 0);
    tl_store *store = NULL;
    CHECK(store_create(database, &store) == TL_OK);
    replacement_and_rollback(store);
    directory_replacement(store);
    fallback_and_legacy(store);
    store_destroy(store);
    CHECK(store_create(database, &store) == TL_OK);
    CHECK(saved_id(store, "/fallback/replaced") != 0);
    store_destroy(store);
    CHECK(unlink(database) == 0);
}
