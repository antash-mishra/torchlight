/* Post-commit publication failures, bounded history and overflow reconciliation. */
#include "test.h"
#include "torchlight/store.h"
#include "torchlight/writer.h"
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct faults {
    atomic_size_t calls;
    atomic_bool hold, entered, allow, overflow;
};
static void pause_briefly(void) {
    const struct timespec delay = {0, 1000000};
    int code = nanosleep(&delay, NULL);
    (void)code;
}
static tl_status faulty_publish(void *context, tl_catalog *catalog,
                                tl_catalog_snapshot **snapshot) {
    struct faults *fault = context;
    size_t call = atomic_fetch_add(&fault->calls, 1);
    if (call != 0 && !atomic_load(&fault->allow)) {
        atomic_store(&fault->entered, true);
        while (atomic_load(&fault->hold))
            pause_briefly();
        return TL_IO;
    }
    return catalog_publish(catalog, snapshot);
}
static tl_status replay_overflow(void *context, tl_watch *watch, tl_watch_callback callback,
                                 void *callback_context) {
    struct faults *fault = context;
    if (atomic_exchange(&fault->overflow, false)) {
        struct inotify_event event = {.wd = -1, .mask = IN_Q_OVERFLOW};
        tl_status status = watch_feed(watch, &event, sizeof(event), callback, callback_context);
        if (status != TL_OK)
            return status;
    }
    return watch_drain(watch, callback, callback_context);
}
static void create_file(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
}
static bool has_query(tl_catalog *catalog, const char *query) {
    tl_catalog_reader *reader = NULL;
    CHECK(catalog_acquire(catalog, &reader) == TL_OK);
    tl_result results[4];
    size_t count = 0;
    CHECK(catalog_query(reader, query, results, 4, &count) == TL_OK);
    catalog_release(reader);
    return count != 0;
}
static tl_writer_stats current_stats(tl_writer *writer) {
    tl_writer_stats stats;
    CHECK(writer_stats(writer, &stats) == TL_OK);
    return stats;
}
static tl_status loaded_count(void *context, const tl_store_entry *entry) {
    size_t *count = context;
    (void)entry;
    (*count)++;
    return TL_OK;
}
/* Whether path itself is indexed: an exact raw path ranks first. */
static bool has_path(tl_catalog *catalog, const char *path) {
    tl_catalog_reader *reader = NULL;
    CHECK(catalog_acquire(catalog, &reader) == TL_OK);
    tl_result results[1];
    size_t count = 0;
    CHECK(catalog_query(reader, path, results, 1, &count) == TL_OK);
    bool found = count == 1 && strcmp(results[0].path, path) == 0;
    catalog_release(reader);
    return found;
}
/* Watch events publish deltas until the delta outgrows its bound, then a full
 * rebuild compacts it; every file stays queryable and removals hide entries. */
static void delta_compaction(void) {
    char directory[] = "/tmp/torchlight-delta-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char root[256], database[256], config_path[256];
    CHECK(snprintf(root, sizeof(root), "%s/root", directory) > 0 && mkdir(root, 0700) == 0);
    CHECK(snprintf(database, sizeof(database), "%s/catalog.db", directory) > 0);
    CHECK(snprintf(config_path, sizeof(config_path), "%s/config", directory) > 0);
    FILE *config_file = fopen(config_path, "w");
    CHECK(config_file != NULL && fprintf(config_file, "root = %s\n", root) > 0 &&
          fclose(config_file) == 0);
    tl_config *config = NULL;
    CHECK(config_create("torchlight", database, config_path, NULL, &config) == TL_OK);
    tl_catalog *catalog = NULL;
    CHECK(catalog_create(2, &catalog) == TL_OK);
    tl_writer_options options = {.config = config,
                                 .catalog = catalog,
                                 .socket_path = "/tmp/unused-delta.sock",
                                 .watch_capacity = 16,
                                 .max_entries = 100,
                                 .max_path_bytes = 8192,
                                 .readers = 1,
                                 .rescan_ms = 3600000,
                                 .history_days = 30,
                                 .delta_entries = 2};
    tl_writer *writer = NULL;
    CHECK(writer_create(&options, &writer) == TL_OK);
    for (size_t i = 0; i < 10000 && current_stats(writer).reconciliations == 0; i++)
        pause_briefly();
    uint64_t builds = current_stats(writer).full_builds;
    char files[6][256];
    for (int i = 0; i < 6; i++) {
        CHECK(snprintf(files[i], sizeof(files[i]), "%s/root/delta-file-%d.txt", directory, i) > 0);
        create_file(files[i]);
        for (size_t wait = 0; wait < 10000 && !has_path(catalog, files[i]); wait++)
            pause_briefly();
        CHECK(has_path(catalog, files[i]));
    }
    tl_writer_stats stats = current_stats(writer);
    CHECK(stats.delta_publications >= 2 && stats.full_builds > builds);
    for (int i = 0; i < 6; i++)
        CHECK(has_path(catalog, files[i])); /* compaction kept earlier deltas */
    CHECK(unlink(files[5]) == 0);
    for (size_t wait = 0; wait < 10000 && has_path(catalog, files[5]); wait++)
        pause_briefly();
    CHECK(!has_path(catalog, files[5]) && has_path(catalog, files[4]));
    writer_destroy(writer);
    CHECK(catalog_destroy(catalog) == TL_OK);
    config_destroy(config);
    for (int i = 0; i < 5; i++)
        CHECK(unlink(files[i]) == 0);
    CHECK(unlink(config_path) == 0 && unlink(database) == 0 && rmdir(root) == 0 &&
          rmdir(directory) == 0);
}
void test_writer(void) {
    delta_compaction();
    char directory[] = "/tmp/torchlight-writer-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char root[256], sub[256], first[256], second[256], overflow_path[256], database[256],
        config_path[256];
    CHECK(snprintf(root, sizeof(root), "%s/root", directory) > 0 && mkdir(root, 0700) == 0);
    CHECK(snprintf(sub, sizeof(sub), "%s/root/sub", directory) > 0 && mkdir(sub, 0700) == 0);
    CHECK(snprintf(first, sizeof(first), "%s/root/first", directory) > 0);
    CHECK(snprintf(second, sizeof(second), "%s/root/second", directory) > 0);
    CHECK(snprintf(overflow_path, sizeof(overflow_path), "%s/root/sub/overflow.txt", directory) >
          0);
    CHECK(snprintf(database, sizeof(database), "%s/catalog.db", directory) > 0);
    CHECK(snprintf(config_path, sizeof(config_path), "%s/config", directory) > 0);
    create_file(first);
    FILE *file = fopen(config_path, "w");
    CHECK(file != NULL && fprintf(file, "root = %s\n", root) > 0 && fclose(file) == 0);
    tl_config *config = NULL;
    CHECK(config_create("torchlight", database, config_path, NULL, &config) == TL_OK);
    tl_catalog *catalog = NULL;
    CHECK(catalog_create(2, &catalog) == TL_OK);
    struct faults faults;
    atomic_init(&faults.calls, 0);
    atomic_init(&faults.hold, true);
    atomic_init(&faults.entered, false);
    atomic_init(&faults.allow, false);
    atomic_init(&faults.overflow, false);
    tl_writer_options options = {.config = config,
                                 .catalog = catalog,
                                 .socket_path = "/tmp/unused-writer.sock",
                                 .watch_capacity = 1,
                                 .max_entries = 100,
                                 .max_path_bytes = 4096,
                                 .readers = 2,
                                 .rescan_ms = 60000,
                                 .history_days = 30,
                                 .history = true,
                                 .publish = faulty_publish,
                                 .publish_context = &faults,
                                 .drain = replay_overflow,
                                 .drain_context = &faults};
    tl_writer *writer = NULL;
    CHECK(writer_create(&options, &writer) == TL_OK);
    for (size_t i = 0; i < 10000 && !atomic_load(&faults.entered); i++)
        pause_briefly();
    CHECK(atomic_load(&faults.entered));
    tl_catalog_reader *old = NULL;
    CHECK(catalog_acquire(catalog, &old) == TL_OK && catalog_reader_gen(old) == 0);
    CHECK(!has_query(catalog, "first"));
    /* The commit succeeded while publication was blocked: the old view must
     * continue serving, and another catalog batch must not overtake recovery. */
    tl_store *reader = NULL;
    CHECK(store_create(database, &reader) == TL_OK);
    size_t count = 0;
    uint64_t catalog_gen = 0;
    CHECK(store_load_catalog(reader, loaded_count, &count, &catalog_gen) == TL_OK &&
          catalog_gen == 1 && count == 3);
    create_file(second);
    /* Publication is still blocked, yet history keeps draining because the
     * persistence thread owns the write connection on its own. Enqueueing is
     * faster than SQLite writes, so the ring saturates exactly once. */
    size_t accepted = 0;
    tl_status enqueued = TL_OK;
    for (size_t i = 0; i < 100000 && enqueued == TL_OK; i++) {
        tl_ipc_request request = {.operation = IPC_QUERY};
        char search_id[64];
        memcpy(request.query, "first", 6);
        CHECK(snprintf(search_id, sizeof(search_id), "search-%zu", i) > 0);
        enqueued = writer_history(writer, &request, search_id);
        accepted += enqueued == TL_OK;
    }
    CHECK(enqueued == TL_LIMIT && accepted >= WRITER_HISTORY_CAPACITY);
    for (size_t i = 0; i < 10000 && current_stats(writer).history_written < accepted; i++)
        pause_briefly();
    tl_writer_stats drained = current_stats(writer);
    CHECK(drained.history_written == accepted && drained.history_pending == 0 &&
          drained.history_dropped == 1 && drained.history_failures == 0);
    CHECK(atomic_load(&faults.hold) && !has_query(catalog, "first"));
    atomic_store(&faults.hold, false);
    for (size_t i = 0; i < 10000 && !current_stats(writer).recovering; i++)
        pause_briefly();
    CHECK(current_stats(writer).recovering && !has_query(catalog, "first"));
    CHECK(writer_reconcile(writer) == TL_OK);
    count = 0;
    CHECK(store_load_catalog(reader, loaded_count, &count, &catalog_gen) == TL_OK &&
          catalog_gen == 1 && count == 3);
    atomic_store(&faults.allow, true);
    CHECK(writer_reconcile(writer) == TL_OK);
    for (size_t i = 0; i < 10000 && !has_query(catalog, "first"); i++)
        pause_briefly();
    CHECK(has_query(catalog, "first") && catalog_reader_gen(old) == 0);
    tl_result results[4];
    count = 99;
    CHECK(catalog_query(old, "first", results, 4, &count) == TL_OK && count == 0);
    catalog_release(old);
    CHECK(writer_reconcile(writer) == TL_OK);
    for (size_t i = 0; i < 10000 && !has_query(catalog, "second"); i++)
        pause_briefly();
    CHECK(has_query(catalog, "second"));
    for (size_t i = 0; i < 10000 && current_stats(writer).indexing; i++)
        pause_briefly();
    CHECK(current_stats(writer).watch_degraded);
    create_file(overflow_path);
    CHECK(!has_query(catalog, "overflow.txt"));
    atomic_store(&faults.overflow, true);
    for (size_t i = 0; i < 10000 && !has_query(catalog, "overflow.txt"); i++)
        pause_briefly();
    CHECK(has_query(catalog, "overflow.txt") && current_stats(writer).watch_overflows >= 1);
    writer_destroy(writer);
    store_destroy(reader);
    CHECK(catalog_destroy(catalog) == TL_OK);
    config_destroy(config);
    CHECK(unlink(first) == 0 && unlink(second) == 0 && unlink(overflow_path) == 0 &&
          unlink(config_path) == 0 && unlink(database) == 0 && rmdir(sub) == 0 &&
          rmdir(root) == 0 && rmdir(directory) == 0);
}
