/* Writer history persistence (M5): the startup usage rebuild and its one-time
 * handover, counters when SQLite loses a whole history batch, and the cost of
 * draining history while another connection holds the database lock. */
#include "test.h"
#include "torchlight/store.h"
#include "torchlight/writer.h"
#include <fcntl.h>
#include <sqlite3.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* SQLite's busy timeout on every store connection (store_create). */
enum { HISTORY_BUSY_MS = 3000 };
struct fixture {
    char base[64], root[128], file[160], database[128], config_path[128];
    tl_config *config;
    tl_catalog *catalog;
    tl_writer *writer;
};
static void pause_ms(long milliseconds) {
    const struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    CHECK(nanosleep(&delay, NULL) == 0);
}
static double now_ms(void) {
    struct timespec now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec * 1e3 + (double)now.tv_nsec / 1e6;
}
static tl_writer_stats stats(tl_writer *writer) {
    tl_writer_stats current;
    CHECK(writer_stats(writer, &current) == TL_OK);
    return current;
}
/* SQL on a separate connection: fault injection and rows only other tools
 * would write. */
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
static void enqueue_search(tl_writer *writer, const char *search_id, const char *query) {
    tl_ipc_request request = {.operation = IPC_QUERY};
    CHECK(strlen(query) < sizeof(request.query));
    memcpy(request.query, query, strlen(query) + 1);
    CHECK(writer_history(writer, &request, search_id) == TL_OK);
}
/* Whether count more history events were written or failed since before. */
static bool drained(tl_writer_stats current, tl_writer_stats before, uint64_t count) {
    uint64_t done = current.history_written - before.history_written + current.history_failures -
                    before.history_failures;
    return done >= count && current.history_pending == 0;
}
static tl_writer_stats wait_for_history(tl_writer *writer, tl_writer_stats before, uint64_t count) {
    tl_writer_stats current = stats(writer);
    for (size_t i = 0; i < 4000 && !drained(current, before, count); i++) {
        pause_ms(10);
        current = stats(writer);
    }
    CHECK(drained(current, before, count));
    return current;
}
/* A database with retained desktop opens: one good, and one with an empty
 * desktop id that the store never writes (another tool could). */
static void seed_history(const struct fixture *fixture) {
    tl_store *store = NULL;
    CHECK(store_create(fixture->database, &store) == TL_OK);
    store_destroy(store);
    char sql[256];
    long long recent = (long long)time(NULL) - 60;
    CHECK(snprintf(sql, sizeof(sql),
                   "INSERT INTO desktop_opens VALUES('kept','b.desktop',NULL,%lld),"
                   "('empty-id','',NULL,%lld)",
                   recent, recent + 1) > 0);
    raw_sql(fixture->database, sql);
}
static void start_fixture(struct fixture *fixture) {
    CHECK(snprintf(fixture->base, sizeof(fixture->base), "/tmp/torchlight-history-XXXXXX") > 0);
    CHECK(mkdtemp(fixture->base) != NULL);
    CHECK(snprintf(fixture->root, sizeof(fixture->root), "%s/root", fixture->base) > 0);
    CHECK(mkdir(fixture->root, 0700) == 0);
    CHECK(snprintf(fixture->file, sizeof(fixture->file), "%s/notes.txt", fixture->root) > 0);
    CHECK(snprintf(fixture->database, sizeof(fixture->database), "%s/catalog.db", fixture->base) >
          0);
    CHECK(snprintf(fixture->config_path, sizeof(fixture->config_path), "%s/config", fixture->base) >
          0);
    int fd = open(fixture->file, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    FILE *config_file = fopen(fixture->config_path, "w");
    CHECK(config_file != NULL && fprintf(config_file, "root = %s\n", fixture->root) > 0);
    CHECK(fclose(config_file) == 0);
    seed_history(fixture);
    CHECK(config_create("torchlight", fixture->database, fixture->config_path, NULL,
                        &fixture->config) == TL_OK);
    CHECK(catalog_create(2, &fixture->catalog) == TL_OK);
    /* No periodic rescans: nothing but the test's history needs the lock. */
    tl_writer_options options = {.config = fixture->config,
                                 .catalog = fixture->catalog,
                                 .socket_path = "/tmp/torchlight-unused-history.sock",
                                 .watch_capacity = 32,
                                 .max_entries = 100,
                                 .max_path_bytes = 4096,
                                 .readers = 1,
                                 .rescan_ms = 3600000,
                                 .history_days = 30,
                                 .history = true};
    CHECK(writer_create(&options, &fixture->writer) == TL_OK);
}
static void stop_fixture(struct fixture *fixture) {
    writer_destroy(fixture->writer);
    CHECK(catalog_destroy(fixture->catalog) == TL_OK);
    config_destroy(fixture->config);
    char path[160];
    static const char *const suffixes[] = {"-wal", "-shm"};
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        CHECK(snprintf(path, sizeof(path), "%s%s", fixture->database, suffixes[i]) > 0);
        (void)unlink(path); /* present only while SQLite keeps them */
    }
    CHECK(unlink(fixture->file) == 0 && unlink(fixture->config_path) == 0);
    CHECK(unlink(fixture->database) == 0);
    CHECK(rmdir(fixture->root) == 0 && rmdir(fixture->base) == 0);
}
/* The startup rebuild skips the bad row and keeps the good one, and the
 * summary is handed over exactly once. Regression: one bad row failed the
 * whole load, so personal ranking started empty. */
static void startup_usage(tl_writer *writer) {
    tl_usage *usage = NULL;
    bool offered = false;
    for (size_t i = 0; i < 1000 && !offered; i++) {
        offered = writer_take_usage(writer, &usage);
        if (!offered)
            pause_ms(10);
    }
    CHECK(offered && usage != NULL && usage_count(usage) == 1);
    tl_usage_target target = usage_target(usage, 0);
    CHECK(target.file_id == 0 && strcmp(target.desktop_id, "b.desktop") == 0);
    usage_destroy(usage);
    usage = (tl_usage *)1;
    CHECK(writer_take_usage(writer, &usage) && usage == NULL);
}
/* SQLite rolls back the whole batch at "boom" (an injected trigger, like an
 * I/O error): the event before it is lost with the batch and counted as
 * failed, and the event after it starts a new batch and is counted as
 * written. Regression: it was saved alone but counted as failed. */
static void lost_batch(const struct fixture *fixture) {
    raw_sql(fixture->database, "CREATE TRIGGER lose_batch BEFORE INSERT ON searches "
                               "WHEN NEW.query = 'boom' BEGIN SELECT RAISE(ROLLBACK, 'x'); END");
    tl_store *locker = NULL;
    CHECK(store_create(fixture->database, &locker) == TL_OK);
    /* Holding the write lock briefly keeps the three events in one drain. */
    CHECK(store_history_begin(locker) == TL_OK);
    tl_writer_stats before = stats(fixture->writer);
    enqueue_search(fixture->writer, "lost-1", "before");
    enqueue_search(fixture->writer, "lost-2", "boom");
    enqueue_search(fixture->writer, "lost-3", "after");
    pause_ms(300);
    CHECK(store_history_rollback(locker) == TL_OK);
    store_destroy(locker);
    tl_writer_stats after = wait_for_history(fixture->writer, before, 3);
    CHECK(after.history_written - before.history_written == 1);
    CHECK(after.history_failures - before.history_failures == 2);
    CHECK(raw_count(fixture->database, "SELECT count(*) FROM searches WHERE id LIKE 'lost-%'") ==
          1);
    CHECK(raw_count(fixture->database, "SELECT count(*) FROM searches WHERE id = 'lost-3'") == 1);
}
/* While another connection holds the write lock, a drain tries to begin its
 * batch once, then writes each event alone: each wait is one busy timeout,
 * three for two events. Regression: retrying the begin before every event
 * doubled the stall (four timeouts here, about two per event). */
static void locked_drain(const struct fixture *fixture) {
    enum { EVENTS = 2 };
    tl_store *locker = NULL;
    CHECK(store_create(fixture->database, &locker) == TL_OK);
    CHECK(store_history_begin(locker) == TL_OK);
    tl_writer_stats before = stats(fixture->writer);
    double start = now_ms();
    enqueue_search(fixture->writer, "locked-1", "first");
    enqueue_search(fixture->writer, "locked-2", "second");
    tl_writer_stats after = wait_for_history(fixture->writer, before, EVENTS);
    double elapsed = now_ms() - start;
    CHECK(store_history_rollback(locker) == TL_OK);
    store_destroy(locker);
    CHECK(after.history_failures - before.history_failures == EVENTS);
    fprintf(stderr, "locked drain of %d events took %.1f s\n", EVENTS, elapsed / 1e3);
    CHECK(elapsed < (EVENTS + 1.5) * HISTORY_BUSY_MS);
}
void test_writer_history(void) {
    struct fixture fixture = {0};
    start_fixture(&fixture);
    startup_usage(fixture.writer);
    /* Let the initial index commit first, so only history needs the lock. */
    for (size_t i = 0; i < 1000 && stats(fixture.writer).full_builds == 0; i++)
        pause_ms(10);
    CHECK(stats(fixture.writer).full_builds != 0);
    pause_ms(200);
    lost_batch(&fixture);
    locked_drain(&fixture);
    stop_fixture(&fixture);
}
