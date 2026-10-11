/* Delta publication keeps deletions incremental and bounds live entries, with
 * exact threshold, abandonment and snapshot-lifetime regressions. */
#include "test.h"
#include "torchlight/delta.h"
#include <string.h>
#include <unistd.h>

struct fixture {
    char database[64];
    tl_store *store;
    tl_catalog *catalog;
    tl_delta *delta;
    tl_delta_limits limits;
};
struct build {
    tl_lexical *engine;
    size_t entries, bytes;
};
static tl_status build_row(void *context, const tl_store_entry *entry) {
    struct build *build = context;
    build->entries++;
    build->bytes += strlen(entry->path) + 1;
    return lexical_add_entry(build->engine, entry->id, entry->path, entry->is_root, entry->is_dir);
}
static void full_build(struct fixture *fixture) {
    struct build build = {0};
    uint64_t catalog_gen = 0;
    tl_catalog_snapshot *snapshot = NULL;
    CHECK(lexical_create(&build.engine) == TL_OK);
    CHECK(store_load_catalog(fixture->store, build_row, &build, &catalog_gen) == TL_OK);
    CHECK(lexical_finish(build.engine) == TL_OK);
    CHECK(catalog_snapshot_create(&build.engine, catalog_gen, fixture->limits.readers, &snapshot) ==
          TL_OK);
    CHECK(catalog_publish(fixture->catalog, &snapshot) == TL_OK);
    delta_reset(fixture->delta, catalog_gen, build.entries, build.bytes);
}
static void put(tl_store *store, const char *path, bool root) {
    tl_crawl_entry entry = {.path = path, .has_stat = true, .is_dir = root, .is_root = root};
    CHECK(store_put(store, &entry) == TL_OK);
}
static void start(struct fixture *fixture) {
    *fixture = (struct fixture){.limits = {.max_entries = 100,
                                           .max_path_bytes = 4096,
                                           .readers = 2,
                                           .min_entries = 2,
                                           .divisor = 32}};
    CHECK(snprintf(fixture->database, sizeof(fixture->database),
                   "/tmp/torchlight-delta-test-XXXXXX") > 0);
    int fd = mkstemp(fixture->database);
    CHECK(fd >= 0 && close(fd) == 0);
    CHECK(store_create(fixture->database, &fixture->store) == TL_OK);
    CHECK(store_track_changes(fixture->store, 32) == TL_OK);
    CHECK(catalog_create(4, &fixture->catalog) == TL_OK);
    CHECK(delta_create(&fixture->delta) == TL_OK && store_begin(fixture->store) == TL_OK);
    const char *paths[] = {"/d", "/d/a", "/d/b", "/d/c", "/d/keep"};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        put(fixture->store, paths[i], i == 0);
    CHECK(store_prune(fixture->store, "/d") == TL_OK && store_commit(fixture->store) == TL_OK);
    full_build(fixture);
}
static void finish(struct fixture *fixture) {
    delta_destroy(fixture->delta);
    CHECK(catalog_destroy(fixture->catalog) == TL_OK);
    store_destroy(fixture->store);
    CHECK(unlink(fixture->database) == 0);
}
static void keep_only(struct fixture *fixture, const char *const *paths, size_t count) {
    CHECK(store_begin(fixture->store) == TL_OK);
    for (size_t i = 0; i < count; i++)
        CHECK(store_keep(fixture->store, paths[i]) == TL_OK);
    CHECK(store_prune_children(fixture->store, "/d") == TL_OK);
    CHECK(store_commit(fixture->store) == TL_OK);
}
static tl_status prepare(struct fixture *fixture, tl_catalog_snapshot **out) {
    const uint64_t *ids = NULL;
    size_t count = 0;
    bool complete = false, roots = false;
    tl_catalog_snapshot *source = NULL;
    CHECK(store_changes(fixture->store, &ids, &count, &complete, &roots) == TL_OK);
    CHECK(complete && !roots && catalog_pin(fixture->catalog, &source) == TL_OK);
    tl_status status =
        delta_prepare(fixture->delta, fixture->store, source, ids, count, &fixture->limits, out);
    catalog_unpin(source);
    return status;
}
static void accept(struct fixture *fixture, size_t tombstones, size_t entries) {
    tl_catalog_snapshot *snapshot = NULL;
    CHECK(prepare(fixture, &snapshot) == TL_OK);
    CHECK(catalog_snapshot_tombstones(snapshot) == tombstones);
    CHECK(catalog_snapshot_count(snapshot) == entries);
    CHECK(catalog_publish(fixture->catalog, &snapshot) == TL_OK);
    delta_commit(fixture->delta);
    CHECK(catalog_reclaim(fixture->catalog) == 0); /* same base */
}
static void needs_compaction(struct fixture *fixture) {
    uint64_t previous = delta_gen(fixture->delta);
    tl_catalog_snapshot *snapshot = NULL;
    CHECK(prepare(fixture, &snapshot) == TL_LIMIT && snapshot == NULL);
    CHECK(delta_gen(fixture->delta) == previous);
}
/* A deletion batch larger than the live delta bound still publishes a delta. */
static void batch_deletions(void) {
    struct fixture fixture;
    start(&fixture);
    const char *keep[] = {"/d/keep"};
    keep_only(&fixture, keep, 1);
    accept(&fixture, 3, 2);
    finish(&fixture);
}
/* Accumulated tombstones do not trigger compaction. Old readers retain their
 * view while new readers see the deletions, even across later reclamation. */
static void accumulated_deletions(void) {
    struct fixture fixture;
    start(&fixture);
    tl_catalog_reader *old = NULL;
    CHECK(catalog_acquire(fixture.catalog, &old) == TL_OK);
    const char *keep[] = {"/d/b", "/d/c", "/d/keep"};
    keep_only(&fixture, keep, 3);
    accept(&fixture, 1, 4);
    keep_only(&fixture, keep + 1, 2);
    accept(&fixture, 2, 3);
    keep_only(&fixture, keep + 2, 1);
    accept(&fixture, 3, 2);
    tl_result results[8];
    size_t count = 0;
    CHECK(catalog_query(old, "/d/a", results, 8, &count) == TL_OK && count != 0);
    CHECK(strcmp(results[0].path, "/d/a") == 0);
    tl_catalog_reader *current = NULL;
    const char *path = NULL;
    CHECK(catalog_acquire(fixture.catalog, &current) == TL_OK);
    CHECK(catalog_resolve(current, results[0].id, &path) == TL_STATE);
    CHECK(catalog_query(current, "keep", results, 8, &count) == TL_OK && count == 1);
    catalog_release(current);
    catalog_release(old);
    CHECK(catalog_reclaim(fixture.catalog) == 0); /* active view still shares the base */
    finish(&fixture);
}
static void rename_entry(struct fixture *fixture, const char *old_path, const char *new_path) {
    CHECK(store_begin(fixture->store) == TL_OK);
    CHECK(store_move(fixture->store, old_path, new_path) == TL_OK);
    CHECK(store_commit(fixture->store) == TL_OK);
}
static void add_entry(struct fixture *fixture, const char *path) {
    CHECK(store_begin(fixture->store) == TL_OK);
    put(fixture->store, path, false);
    CHECK(store_commit(fixture->store) == TL_OK);
}
/* Live updates and additions count once, including after abandonment. Removed
 * rows do not count, and the next live entry over the bound still compacts. */
static void mixed_changes(void) {
    struct fixture fixture;
    start(&fixture);
    rename_entry(&fixture, "/d/a", "/d/renamed");
    accept(&fixture, 1, 5);
    rename_entry(&fixture, "/d/renamed", "/d/renamed-again");
    accept(&fixture, 1, 5);
    add_entry(&fixture, "/d/new");
    tl_catalog_snapshot *discarded = NULL;
    uint64_t previous = delta_gen(fixture.delta);
    CHECK(prepare(&fixture, &discarded) == TL_OK);
    catalog_snapshot_destroy(discarded);
    delta_abandon(fixture.delta);
    CHECK(delta_gen(fixture.delta) == previous);
    accept(&fixture, 1, 6);
    const char *keep[] = {"/d/b", "/d/renamed-again", "/d/c", "/d/keep"};
    keep_only(&fixture, keep, 4);
    accept(&fixture, 1, 5);
    keep_only(&fixture, keep + 1, 3);
    accept(&fixture, 2, 4);
    add_entry(&fixture, "/d/another");
    accept(&fixture, 2, 5);
    add_entry(&fixture, "/d/over-limit");
    needs_compaction(&fixture);
    full_build(&fixture);
    CHECK(catalog_reclaim(fixture.catalog) == 1);
    finish(&fixture);
}
void test_delta(void) {
    batch_deletions();
    accumulated_deletions();
    mixed_changes();
}
