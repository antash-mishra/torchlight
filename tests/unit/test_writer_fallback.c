/* Instance exhaustion must preserve periodic scans, status and later recovery. */
#include "test.h"
#include "torchlight/writer.h"
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct watch_fault {
    atomic_bool unavailable;
};
struct fixture {
    char base[64], root[128], file[160], fresh[160], database[128], config_path[128];
    tl_config *config;
    tl_catalog *catalog;
    tl_writer *writer;
    struct watch_fault fault;
};
static void pause_briefly(void) {
    const struct timespec delay = {0, 1000000};
    CHECK(nanosleep(&delay, NULL) == 0);
}
static void touch(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
}
static tl_status create_watch(void *context, size_t capacity, tl_watch **out) {
    struct watch_fault *fault = context;
    *out = NULL;
    return atomic_load(&fault->unavailable) ? TL_IO : watch_create(capacity, out);
}
static uint64_t query_id(tl_catalog *catalog, const char *query) {
    tl_catalog_reader *reader = NULL;
    CHECK(catalog_acquire(catalog, &reader) == TL_OK);
    tl_result results[4];
    size_t count = 0;
    CHECK(catalog_query(reader, query, results, 4, &count) == TL_OK);
    uint64_t id = count == 0 ? 0 : results[0].id;
    catalog_release(reader);
    return id;
}
static tl_writer_stats stats(tl_writer *writer) {
    tl_writer_stats current;
    CHECK(writer_stats(writer, &current) == TL_OK);
    return current;
}
static uint64_t wait_for_file(tl_catalog *catalog, const char *query, uint64_t previous) {
    uint64_t id = 0;
    for (size_t i = 0; i < 10000; i++) {
        id = query_id(catalog, query);
        if (id != 0 && id != previous)
            return id;
        pause_briefly();
    }
    CHECK(id != 0 && id != previous);
    return id;
}
static void start_fixture(struct fixture *fixture) {
    CHECK(snprintf(fixture->base, sizeof(fixture->base), "/tmp/torchlight-fallback-XXXXXX") > 0);
    CHECK(mkdtemp(fixture->base) != NULL);
    CHECK(snprintf(fixture->root, sizeof(fixture->root), "%s/root", fixture->base) > 0);
    CHECK(mkdir(fixture->root, 0700) == 0);
    CHECK(snprintf(fixture->file, sizeof(fixture->file), "%s/same.txt", fixture->root) > 0);
    CHECK(snprintf(fixture->fresh, sizeof(fixture->fresh), "%s/fresh.txt", fixture->root) > 0);
    CHECK(snprintf(fixture->database, sizeof(fixture->database), "%s/catalog.db", fixture->base) >
          0);
    CHECK(snprintf(fixture->config_path, sizeof(fixture->config_path), "%s/config", fixture->base) >
          0);
    touch(fixture->file);
    FILE *config_file = fopen(fixture->config_path, "w");
    CHECK(config_file != NULL && fprintf(config_file, "root = %s\n", fixture->root) > 0);
    CHECK(fclose(config_file) == 0);
    CHECK(config_create("torchlight", fixture->database, fixture->config_path, NULL,
                        &fixture->config) == TL_OK);
    CHECK(catalog_create(2, &fixture->catalog) == TL_OK);
    atomic_init(&fixture->fault.unavailable, true);
    tl_writer_options options = {.config = fixture->config,
                                 .catalog = fixture->catalog,
                                 .socket_path = "/tmp/torchlight-unused-fallback.sock",
                                 .watch_capacity = 32,
                                 .max_entries = 100,
                                 .max_path_bytes = 4096,
                                 .readers = 1,
                                 .rescan_ms = 100,
                                 .history_days = 30,
                                 .create_watch = create_watch,
                                 .watch_context = &fixture->fault};
    CHECK(writer_create(&options, &fixture->writer) == TL_OK);
}
static void recover_watch(struct fixture *fixture) {
    atomic_store(&fixture->fault.unavailable, false);
    CHECK(writer_reconcile(fixture->writer) == TL_OK);
    for (size_t i = 0; i < 10000; i++) {
        tl_writer_stats current = stats(fixture->writer);
        if (!current.watch_degraded && !current.indexing)
            break;
        pause_briefly();
    }
    tl_writer_stats healthy = stats(fixture->writer);
    CHECK(!healthy.watch_degraded && healthy.watches == 1 && !healthy.degraded);
    atomic_store(&fixture->fault.unavailable, true);
    CHECK(unlink(fixture->fresh) == 0);
    CHECK(writer_reconcile(fixture->writer) == TL_OK);
    for (size_t i = 0; i < 10000 && query_id(fixture->catalog, "fresh.txt") != 0; i++)
        pause_briefly();
    CHECK(query_id(fixture->catalog, "fresh.txt") == 0);
    tl_writer_stats reduced = stats(fixture->writer);
    CHECK(reduced.watch_degraded && reduced.watches == healthy.watches);
    touch(fixture->fresh);
    CHECK(wait_for_file(fixture->catalog, "fresh.txt", 0) != 0);
}
void test_writer_fallback(void) {
    struct fixture fixture = {0};
    start_fixture(&fixture);
    uint64_t original = wait_for_file(fixture.catalog, "same.txt", 0);
    tl_writer_stats degraded = stats(fixture.writer);
    CHECK(degraded.watch_degraded && degraded.watch_unavailable > 0 && degraded.watches == 0);
    touch(fixture.fresh);
    CHECK(wait_for_file(fixture.catalog, "fresh.txt", 0) != 0);
    int old_file = open(fixture.file, O_RDONLY);
    CHECK(old_file >= 0 && unlink(fixture.file) == 0);
    touch(fixture.file);
    CHECK(wait_for_file(fixture.catalog, "same.txt", original) > original);
    CHECK(close(old_file) == 0);
    CHECK(stats(fixture.writer).reconciliations >= 2);
    recover_watch(&fixture);
    writer_destroy(fixture.writer);
    CHECK(catalog_destroy(fixture.catalog) == TL_OK);
    config_destroy(fixture.config);
    CHECK(unlink(fixture.file) == 0 && unlink(fixture.fresh) == 0);
    CHECK(unlink(fixture.config_path) == 0 && unlink(fixture.database) == 0);
    CHECK(rmdir(fixture.root) == 0 && rmdir(fixture.base) == 0);
}
