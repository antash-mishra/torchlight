/* Snapshot ownership, bounded leases, stale resolution and concurrent retirement. */
#include "test.h"
#include "torchlight/catalog.h"
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static tl_catalog_snapshot *snapshot(uint64_t catalog_gen, const char *path, size_t readers) {
    tl_lexical *engine = NULL;
    tl_catalog_snapshot *view = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(lexical_add(engine, 1, "/r", true) == TL_OK);
    CHECK(lexical_add(engine, catalog_gen + 2, path, false) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(catalog_snapshot_create(&engine, catalog_gen, readers, &view) == TL_OK);
    CHECK(engine == NULL && view != NULL);
    return view;
}
static void expect_path(tl_catalog_reader *reader, const char *query, const char *path) {
    tl_result results[4];
    size_t count = 0;
    CHECK(catalog_query(reader, query, results, 4, &count) == TL_OK);
    CHECK(count > 0 && strcmp(results[0].path, path) == 0);
}
static void ownership_and_limits(void) {
    tl_catalog *catalog = NULL;
    CHECK(catalog_create(0, &catalog) == TL_INVALID && catalog == NULL);
    CHECK(catalog_create(CATALOG_MAX_SNAPSHOTS + 1, &catalog) == TL_INVALID);
    CHECK(catalog_create(2, &catalog) == TL_OK);
    tl_catalog_reader *old = NULL, *other = NULL, *current = NULL;
    CHECK(catalog_acquire(catalog, &old) == TL_STATE && old == NULL);
    tl_catalog_stats stats;
    CHECK(catalog_stats(catalog, &stats) == TL_OK && !stats.available);
    CHECK(stats.snapshots == 0 && stats.readers == 0);
    tl_catalog_snapshot *view = snapshot(0, "/r/projectNotes.md", 2);
    CHECK(catalog_publish(catalog, &view) == TL_OK && view == NULL);
    CHECK(catalog_acquire(catalog, &old) == TL_OK);
    CHECK(catalog_acquire(catalog, &other) == TL_OK);
    CHECK(catalog_acquire(catalog, &current) == TL_LIMIT && current == NULL);
    CHECK(catalog_reader_gen(old) == 0);
    CHECK(catalog_destroy(catalog) == TL_STATE);
    /* The old cache/result path must stay valid through retirement. */
    expect_path(old, "p", "/r/projectNotes.md");
    const char *borrowed = NULL;
    CHECK(catalog_resolve(old, 2, &borrowed) == TL_OK);
    view = snapshot(1, "/r/projectZebra.md", 2);
    CHECK(catalog_publish(catalog, &view) == TL_OK && view == NULL);
    catalog_reclaim(catalog);
    CHECK(strcmp(borrowed, "/r/projectNotes.md") == 0);
    expect_path(old, "prjnts", borrowed);
    CHECK(catalog_acquire(catalog, &current) == TL_OK);
    expect_path(current, "prjz", "/r/projectZebra.md");
    CHECK(catalog_resolve(current, 2, &borrowed) == TL_STATE && borrowed == NULL);
    CHECK(catalog_resolve(current, 3, &borrowed) == TL_OK);
    CHECK(strcmp(borrowed, "/r/projectZebra.md") == 0);
    CHECK(catalog_stats(catalog, &stats) == TL_OK && stats.catalog_gen == 1);
    CHECK(stats.entries == 2 && stats.snapshots == 2 && stats.readers == 3);
    view = snapshot(1, "/r/duplicate.md", 1);
    CHECK(catalog_publish(catalog, &view) == TL_STATE && view != NULL);
    catalog_snapshot_destroy(view);
    view = snapshot(0, "/r/obsolete.md", 1);
    CHECK(catalog_publish(catalog, &view) == TL_STATE);
    catalog_snapshot_destroy(view);
    view = snapshot(2, "/r/new.md", 1);
    CHECK(catalog_publish(catalog, &view) == TL_LIMIT && view != NULL);
    catalog_release(old);
    catalog_reclaim(catalog);
    CHECK(catalog_publish(catalog, &view) == TL_LIMIT); /* second old lease still pins it */
    catalog_release(other);
    catalog_reclaim(catalog);
    CHECK(catalog_publish(catalog, &view) == TL_OK && view == NULL);
    expect_path(current, "prjz", "/r/projectZebra.md");
    catalog_release(current);
    catalog_reclaim(catalog);
    CHECK(catalog_stats(catalog, &stats) == TL_OK && stats.snapshots == 1 && stats.readers == 0);
    CHECK(catalog_destroy(catalog) == TL_OK);
}
static void failed_preparation(void) {
    tl_lexical *engine = NULL;
    tl_catalog_snapshot *view = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    CHECK(catalog_snapshot_create(&engine, 0, 0, &view) == TL_INVALID);
    CHECK(engine != NULL && view == NULL);
    CHECK(catalog_snapshot_create(&engine, 0, CATALOG_MAX_READERS + 1, &view) == TL_INVALID);
    CHECK(catalog_snapshot_create(&engine, 0, 1, &view) == TL_STATE);
    CHECK(engine != NULL && view == NULL); /* failure never consumes the engine */
    CHECK(lexical_finish(engine) == TL_OK);
    CHECK(catalog_snapshot_create(&engine, 0, 1, &view) == TL_OK);
    catalog_snapshot_destroy(view);
    CHECK(engine == NULL);
    tl_catalog *catalog = NULL;
    CHECK(catalog_create(1, &catalog) == TL_OK);
    CHECK(catalog_acquire(NULL, NULL) == TL_INVALID);
    CHECK(catalog_publish(catalog, NULL) == TL_INVALID);
    CHECK(catalog_stats(catalog, NULL) == TL_INVALID);
    CHECK(catalog_resolve(NULL, 1, NULL) == TL_INVALID);
    const char *path = NULL;
    CHECK(catalog_resolve(NULL, 1, &path) == TL_INVALID && path == NULL);
    size_t count = 7;
    CHECK(catalog_query(NULL, "x", NULL, 1, &count) == TL_INVALID && count == 0);
    CHECK(catalog_destroy(catalog) == TL_OK);
    CHECK(catalog_destroy(NULL) == TL_OK);
    catalog_release(NULL);
    catalog_reclaim(NULL);
}
enum { CONCURRENT_READERS = 4, PUBLICATION_ROUNDS = 24, QUERIES_PER_ROUND = 50 };
struct concurrent {
    tl_catalog *catalog;
    pthread_barrier_t barrier;
};
static void synchronize(struct concurrent *context) {
    int code = pthread_barrier_wait(&context->barrier);
    CHECK(code == 0 || code == PTHREAD_BARRIER_SERIAL_THREAD);
}
static void *query_during_publication(void *argument) {
    struct concurrent *context = argument;
    for (uint64_t round = 1; round <= PUBLICATION_ROUNDS; round++) {
        tl_catalog_reader *reader = NULL;
        CHECK(catalog_acquire(context->catalog, &reader) == TL_OK);
        CHECK(catalog_reader_gen(reader) == round);
        char path[64];
        snprintf(path, sizeof(path), "/r/%llu/report.md", (unsigned long long)round);
        synchronize(context); /* every reader is pinned before replacement */
        for (size_t i = 0; i < QUERIES_PER_ROUND; i++)
            expect_path(reader, "report", path);
        synchronize(context); /* replacement and reclamation have completed */
        expect_path(reader, "report", path);
        catalog_release(reader);
        synchronize(context); /* release before writer reclaims */
        synchronize(context); /* reclamation before next acquisition */
    }
    return NULL;
}
static void concurrent_publication(void) {
    struct concurrent context = {0};
    CHECK(catalog_create(2, &context.catalog) == TL_OK);
    tl_catalog_snapshot *view = snapshot(1, "/r/1/report.md", CONCURRENT_READERS);
    CHECK(catalog_publish(context.catalog, &view) == TL_OK);
    CHECK(pthread_barrier_init(&context.barrier, NULL, CONCURRENT_READERS + 1) == 0);
    pthread_t threads[CONCURRENT_READERS];
    for (size_t i = 0; i < CONCURRENT_READERS; i++)
        CHECK(pthread_create(&threads[i], NULL, query_during_publication, &context) == 0);
    for (uint64_t round = 1; round <= PUBLICATION_ROUNDS; round++) {
        synchronize(&context);
        char path[64];
        snprintf(path, sizeof(path), "/r/%llu/report.md", (unsigned long long)(round + 1));
        view = snapshot(round + 1, path, CONCURRENT_READERS);
        CHECK(catalog_publish(context.catalog, &view) == TL_OK);
        catalog_reclaim(context.catalog);
        tl_catalog_stats stats;
        CHECK(catalog_stats(context.catalog, &stats) == TL_OK);
        CHECK(stats.snapshots == 2 && stats.readers == CONCURRENT_READERS);
        synchronize(&context);
        synchronize(&context);
        catalog_reclaim(context.catalog);
        CHECK(catalog_stats(context.catalog, &stats) == TL_OK);
        CHECK(stats.snapshots == 1 && stats.readers == 0);
        synchronize(&context);
    }
    for (size_t i = 0; i < CONCURRENT_READERS; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(pthread_barrier_destroy(&context.barrier) == 0);
    CHECK(catalog_destroy(context.catalog) == TL_OK);
}
struct racing {
    struct concurrent shared;
    atomic_bool stop;
};
/* Acquisition/release now race the pointer swap and garbage collection,
 * complementing the barrier test's deterministic old-view lifetime checks. */
static void *race_acquisition(void *argument) {
    struct racing *context = argument;
    synchronize(&context->shared);
    do {
        tl_catalog_reader *reader = NULL;
        CHECK(catalog_acquire(context->shared.catalog, &reader) == TL_OK);
        uint64_t catalog_gen = catalog_reader_gen(reader);
        char path[64];
        snprintf(path, sizeof(path), "/r/%llu/report.md", (unsigned long long)catalog_gen);
        expect_path(reader, "report", path);
        const char *resolved = NULL;
        CHECK(catalog_resolve(reader, catalog_gen + 2, &resolved) == TL_OK);
        CHECK(strcmp(resolved, path) == 0);
        catalog_release(reader);
    } while (!atomic_load(&context->stop));
    return NULL;
}
static void acquisition_races_reclamation(void) {
    struct racing context = {0};
    atomic_init(&context.stop, false);
    /* At most four independently pinned old views plus active and candidate. */
    CHECK(catalog_create(CONCURRENT_READERS + 2, &context.shared.catalog) == TL_OK);
    tl_catalog_snapshot *view = snapshot(1, "/r/1/report.md", CONCURRENT_READERS);
    CHECK(catalog_publish(context.shared.catalog, &view) == TL_OK);
    CHECK(pthread_barrier_init(&context.shared.barrier, NULL, CONCURRENT_READERS + 1) == 0);
    pthread_t threads[CONCURRENT_READERS];
    for (size_t i = 0; i < CONCURRENT_READERS; i++)
        CHECK(pthread_create(&threads[i], NULL, race_acquisition, &context) == 0);
    synchronize(&context.shared);
    for (uint64_t catalog_gen = 2; catalog_gen <= PUBLICATION_ROUNDS; catalog_gen++) {
        char path[64];
        snprintf(path, sizeof(path), "/r/%llu/report.md", (unsigned long long)catalog_gen);
        view = snapshot(catalog_gen, path, CONCURRENT_READERS);
        catalog_reclaim(context.shared.catalog);
        CHECK(catalog_publish(context.shared.catalog, &view) == TL_OK);
    }
    atomic_store(&context.stop, true);
    for (size_t i = 0; i < CONCURRENT_READERS; i++)
        CHECK(pthread_join(threads[i], NULL) == 0);
    CHECK(pthread_barrier_destroy(&context.shared.barrier) == 0);
    catalog_reclaim(context.shared.catalog);
    tl_catalog_stats stats;
    CHECK(catalog_stats(context.shared.catalog, &stats) == TL_OK);
    CHECK(stats.snapshots == 1 && stats.readers == 0 && stats.catalog_gen == PUBLICATION_ROUNDS);
    CHECK(catalog_destroy(context.shared.catalog) == TL_OK);
}
/* A derived snapshot shares the base, hides retired base entries, merges the
 * delta in one total order and lists live entries in id order; base
 * workspaces bound leases across every snapshot sharing them. */
static void derived_snapshots(void) {
    tl_lexical *engine = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    const char *paths[] = {"/r", "/r/alpha.md", "/r/beta.md", "/r/gamma.md", "/r/alpha.mdz"};
    for (size_t i = 0; i < 5; i++)
        CHECK(lexical_add(engine, i + 1, paths[i], i == 0) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    tl_catalog *catalog = NULL;
    tl_catalog_snapshot *base = NULL, *derived = NULL, *pin = NULL;
    CHECK(catalog_create(3, &catalog) == TL_OK);
    CHECK(catalog_snapshot_create(&engine, 1, 1, &base) == TL_OK);
    CHECK(catalog_publish(catalog, &base) == TL_OK && catalog_pin(catalog, &pin) == TL_OK);
    /* beta (id 3) is renamed to "alpha2.md"; gamma (id 4) is deleted; id 6 is new. */
    tl_lexical *delta = NULL;
    CHECK(lexical_create(&delta) == TL_OK);
    CHECK(lexical_set_reference(delta, catalog_snapshot_base(pin)) == TL_OK);
    CHECK(lexical_add(delta, 3, "/r/alpha2.md", false) == TL_OK);
    CHECK(lexical_add(delta, 6, "/r/alpha.mdx", false) == TL_OK);
    CHECK(lexical_finish(delta) == TL_OK);
    const uint64_t retired[] = {4, 3, 42};
    CHECK(catalog_snapshot_derive(pin, &delta, retired, 3, 1, 1, &derived) == TL_OK);
    CHECK(delta == NULL && catalog_snapshot_tombstones(derived) == 2);
    CHECK(catalog_publish(catalog, &derived) == TL_STATE); /* catalog_gen must advance */
    catalog_snapshot_destroy(derived);
    CHECK(lexical_create(&delta) == TL_OK &&
          lexical_set_reference(delta, catalog_snapshot_base(pin)) == TL_OK);
    CHECK(lexical_add(delta, 3, "/r/alpha2.md", false) == TL_OK);
    CHECK(lexical_add(delta, 6, "/r/alpha.mdx", false) == TL_OK);
    CHECK(lexical_finish(delta) == TL_OK);
    CHECK(catalog_snapshot_derive(pin, &delta, retired, 3, 2, 1, &derived) == TL_OK);
    CHECK(catalog_publish(catalog, &derived) == TL_OK);
    tl_catalog_reader *reader = NULL, *other = NULL;
    CHECK(catalog_acquire(catalog, &reader) == TL_OK);
    CHECK(catalog_acquire(catalog, &other) == TL_LIMIT); /* one shared base workspace */
    tl_result results[8];
    size_t count = 0;
    /* Equal scores across segments order by raw path: .mdx < .mdz < 2.md. */
    CHECK(catalog_query(reader, "alpha", results, 8, &count) == TL_OK && count == 4);
    CHECK(results[0].id == 2 && results[1].id == 6 && results[2].id == 5 && results[3].id == 3);
    CHECK(catalog_query(reader, "alpha", results, 2, &count) == TL_OK && count == 2 &&
          results[1].id == 6);
    CHECK(catalog_query(reader, "beta", results, 8, &count) == TL_OK && count == 0);
    CHECK(catalog_query(reader, "gamma", results, 8, &count) == TL_OK && count == 0);
    const char *path = NULL;
    CHECK(catalog_resolve(reader, 3, &path) == TL_OK && strcmp(path, "/r/alpha2.md") == 0);
    CHECK(catalog_resolve(reader, 4, &path) == TL_STATE &&
          catalog_resolve(reader, 2, &path) == TL_OK);
    CHECK(!catalog_is_dir(reader, 4));
    catalog_release(reader);
    tl_catalog_stats stats;
    CHECK(catalog_stats(catalog, &stats) == TL_OK && stats.entries == 5 && stats.catalog_gen == 2);
    catalog_unpin(pin);
    CHECK(catalog_pin(catalog, &pin) == TL_OK && catalog_snapshot_count(pin) == 5);
    uint64_t expected[] = {1, 2, 3, 5, 6}, id = 0;
    bool directory = false;
    for (size_t i = 0; i < 5; i++) {
        CHECK(catalog_snapshot_entry(pin, i, &id, &path, &directory) == TL_OK && id == expected[i]);
        CHECK(catalog_snapshot_context(pin, i) != NULL);
    }
    CHECK(strcmp(catalog_snapshot_context(pin, 2), "r/alpha2.md") == 0);
    CHECK(catalog_snapshot_entry(pin, 5, &id, &path, &directory) == TL_INVALID);
    catalog_unpin(pin);
    /* Retiring the base snapshot keeps the shared engine alive for the delta. */
    catalog_reclaim(catalog);
    CHECK(catalog_stats(catalog, &stats) == TL_OK && stats.snapshots == 1);
    CHECK(catalog_acquire(catalog, &reader) == TL_OK);
    CHECK(catalog_query(reader, "alpha", results, 8, &count) == TL_OK && count == 4);
    catalog_release(reader);
    tl_lexical *unused = NULL;
    CHECK(catalog_snapshot_derive(NULL, &unused, NULL, 0, 3, 1, &derived) == TL_INVALID);
    CHECK(catalog_destroy(catalog) == TL_OK);
}
/* Boosts reach live ids in either segment, skip retired ids, apply to one
 * query only, and keep their mapping per key. */
static void boosted_snapshots(void) {
    tl_lexical *engine = NULL;
    CHECK(lexical_create(&engine) == TL_OK);
    const char *paths[] = {"/r", "/r/alpha.md", "/r/beta.md", "/r/gamma.md", "/r/alpha.mdz"};
    for (size_t i = 0; i < 5; i++)
        CHECK(lexical_add(engine, i + 1, paths[i], i == 0) == TL_OK);
    CHECK(lexical_finish(engine) == TL_OK);
    tl_catalog *catalog = NULL;
    tl_catalog_snapshot *base = NULL, *derived = NULL, *pin = NULL;
    CHECK(catalog_create(3, &catalog) == TL_OK);
    CHECK(catalog_snapshot_create(&engine, 1, 1, &base) == TL_OK);
    CHECK(catalog_publish(catalog, &base) == TL_OK && catalog_pin(catalog, &pin) == TL_OK);
    tl_lexical *delta = NULL;
    CHECK(lexical_create(&delta) == TL_OK &&
          lexical_set_reference(delta, catalog_snapshot_base(pin)) == TL_OK);
    CHECK(lexical_add(delta, 3, "/r/alpha2.md", false) == TL_OK);
    CHECK(lexical_add(delta, 6, "/r/alpha.mdx", false) == TL_OK);
    CHECK(lexical_finish(delta) == TL_OK);
    const uint64_t retired[] = {4, 3};
    CHECK(catalog_snapshot_derive(pin, &delta, retired, 2, 2, 1, &derived) == TL_OK);
    CHECK(catalog_publish(catalog, &derived) == TL_OK);
    catalog_unpin(pin);
    tl_catalog_reader *reader = NULL;
    CHECK(catalog_acquire(catalog, &reader) == TL_OK);
    tl_result plain[8], results[8];
    size_t plain_count = 0, count = 0;
    CHECK(catalog_query(reader, "alpha", plain, 8, &plain_count) == TL_OK && plain_count == 4);
    CHECK(plain[0].id == 2 && plain[3].id == 3);
    /* id 3 (delta) and id 5 (base) lifted; 4 (retired) and 99 (absent) ignored. */
    const uint64_t ids[] = {3, 4, 5, 99};
    const int values[] = {40, LEXICAL_BOOST_MAX, 20, LEXICAL_BOOST_MAX};
    tl_catalog_boosts boosts = {ids, values, 4, 7};
    CHECK(catalog_query_boosted(reader, "alpha", &boosts, results, 8, &count) == TL_OK);
    CHECK(count == 4 && results[0].id == 3 && results[1].id == 5 && results[2].id == 2);
    CHECK(results[0].score == plain[3].score + 40 && results[1].score == plain[2].score + 20);
    CHECK(catalog_query_boosted(reader, "gamma", &boosts, results, 8, &count) == TL_OK &&
          count == 0);
    /* Same key reuses the mapping; boosts never outlive their query. */
    CHECK(catalog_query_boosted(reader, "alpha", &boosts, results, 1, &count) == TL_OK &&
          count == 1 && results[0].id == 3);
    CHECK(catalog_query(reader, "alpha", results, 8, &count) == TL_OK && count == 4);
    for (size_t i = 0; i < count; i++)
        CHECK(results[i].id == plain[i].id && results[i].score == plain[i].score);
    /* A new key remaps the ids. */
    const uint64_t other_ids[] = {6};
    const int other_values[] = {2}; /* alpha.md leads alpha.mdx by one length point */
    tl_catalog_boosts other = {other_ids, other_values, 1, 8};
    CHECK(catalog_query_boosted(reader, "alpha", &other, results, 8, &count) == TL_OK);
    CHECK(results[0].id == 6 && results[1].id == 2);
    const int invalid_values[] = {LEXICAL_BOOST_MAX + 1};
    tl_catalog_boosts invalid = {other_ids, invalid_values, 1, 9};
    CHECK(catalog_query_boosted(reader, "alpha", &invalid, results, 8, &count) == TL_INVALID &&
          count == 0);
    tl_catalog_boosts missing = {NULL, NULL, 1, 10};
    CHECK(catalog_query_boosted(reader, "alpha", &missing, results, 8, &count) == TL_INVALID);
    tl_catalog_boosts large = {ids, values, LEXICAL_MAX_BOOSTED + 1, 11};
    CHECK(catalog_query_boosted(reader, "alpha", &large, results, 8, &count) == TL_LIMIT);
    catalog_release(reader);
    /* A new lease of the shared base workspace starts without boosts. */
    CHECK(catalog_acquire(catalog, &reader) == TL_OK);
    CHECK(catalog_query(reader, "alpha", results, 8, &count) == TL_OK && results[0].id == 2);
    catalog_release(reader);
    CHECK(catalog_destroy(catalog) == TL_OK);
}
void test_catalog(void) {
    derived_snapshots();
    boosted_snapshots();
    tl_catalog *registry = NULL;
    CHECK(catalog_create(2, &registry) == TL_OK);
    tl_catalog_snapshot *view = snapshot(1, "/r/old.md", 1), *pin = NULL;
    CHECK(catalog_publish(registry, &view) == TL_OK);
    CHECK(catalog_pin(registry, &pin) == TL_OK);
    tl_catalog_reader *reader = NULL;
    CHECK(catalog_acquire(registry, &reader) == TL_OK);
    catalog_release(reader);
    view = snapshot(2, "/r/new.md", 1);
    CHECK(catalog_publish(registry, &view) == TL_OK);
    catalog_reclaim(registry);
    CHECK(catalog_snapshot_gen(pin) == 1 && catalog_snapshot_count(pin) == 2);
    uint64_t id = 0;
    const char *path = NULL;
    bool directory = false;
    CHECK(catalog_snapshot_entry(pin, 1, &id, &path, &directory) == TL_OK);
    CHECK(id == 3 && strcmp(path, "/r/old.md") == 0 && !directory);
    CHECK(strcmp(catalog_snapshot_context(pin, 0), "r") == 0);
    CHECK(strcmp(catalog_snapshot_context(pin, 1), "r/old.md") == 0);
    CHECK(catalog_destroy(registry) == TL_STATE);
    catalog_unpin(pin);
    catalog_reclaim(registry);
    CHECK(catalog_destroy(registry) == TL_OK);
    ownership_and_limits();
    failed_preparation();
    concurrent_publication();
    acquisition_races_reclamation();
}
