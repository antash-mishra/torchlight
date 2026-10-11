/* M7 repair scheduling: no periodic full scans while inotify covers every
 * directory, scoped repairs of unwatched subtrees, the backstop and mount
 * signals, and memory release after a retired full build. */
#include "test.h"
#include "torchlight/writer.h"
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define WAIT_STEPS 10000
#define QUIET_WINDOW_MS 600
struct fixture {
    char base[64], root[128], sub[160], database[128], config_path[128];
    tl_config *config;
    tl_catalog *catalog;
    tl_writer *writer;
    tl_writer_options options;
};
static void pause_ms(long milliseconds) {
    const struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    CHECK(nanosleep(&delay, NULL) == 0);
}
static void touch(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
}
static tl_writer_stats stats(const struct fixture *fixture) {
    tl_writer_stats current;
    CHECK(writer_stats(fixture->writer, &current) == TL_OK);
    return current;
}
static uint64_t full_scans(const tl_writer_stats *current) {
    return current->reconciliations - current->scoped_reconciliations;
}
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
static void wait_path(const struct fixture *fixture, const char *path) {
    for (size_t i = 0; i < WAIT_STEPS && !has_path(fixture->catalog, path); i++)
        pause_ms(1);
    CHECK(has_path(fixture->catalog, path));
}
/* Wait until the writer is idle after at least minimum reconciliations. */
static tl_writer_stats wait_idle(const struct fixture *fixture, uint64_t minimum) {
    for (size_t i = 0; i < WAIT_STEPS; i++) {
        tl_writer_stats current = stats(fixture);
        if (!current.indexing && current.reconciliations >= minimum)
            return current;
        pause_ms(1);
    }
    CHECK(false);
    return stats(fixture);
}
static void wait_reason(const struct fixture *fixture, tl_writer_full_reason reason) {
    for (size_t i = 0; i < WAIT_STEPS && stats(fixture).last_full_reason != reason; i++)
        pause_ms(1);
    CHECK(stats(fixture).last_full_reason == reason);
}
/* A root with one subdirectory; options default to a quiet healthy writer. */
static void prepare(struct fixture *fixture) {
    *fixture = (struct fixture){0};
    CHECK(snprintf(fixture->base, sizeof(fixture->base), "/tmp/torchlight-repair-XXXXXX") > 0);
    CHECK(mkdtemp(fixture->base) != NULL);
    CHECK(snprintf(fixture->root, sizeof(fixture->root), "%s/root", fixture->base) > 0);
    CHECK(snprintf(fixture->sub, sizeof(fixture->sub), "%s/sub", fixture->root) > 0);
    CHECK(mkdir(fixture->root, 0700) == 0 && mkdir(fixture->sub, 0700) == 0);
    CHECK(snprintf(fixture->database, sizeof(fixture->database), "%s/catalog.db", fixture->base) >
          0);
    CHECK(snprintf(fixture->config_path, sizeof(fixture->config_path), "%s/config", fixture->base) >
          0);
    FILE *config_file = fopen(fixture->config_path, "w");
    CHECK(config_file != NULL && fprintf(config_file, "root = %s\n", fixture->root) > 0);
    CHECK(fclose(config_file) == 0);
    CHECK(config_create("torchlight", fixture->database, fixture->config_path, NULL,
                        &fixture->config) == TL_OK);
    CHECK(catalog_create(2, &fixture->catalog) == TL_OK);
    fixture->options = (tl_writer_options){.config = fixture->config,
                                           .catalog = fixture->catalog,
                                           .socket_path = "/tmp/torchlight-unused-repair.sock",
                                           .watch_capacity = 32,
                                           .max_entries = 100,
                                           .max_path_bytes = 4096,
                                           .readers = 1,
                                           .rescan_ms = 100,
                                           .history_days = 30};
}
static void start(struct fixture *fixture) {
    CHECK(writer_create(&fixture->options, &fixture->writer) == TL_OK);
}
/* Remove every file the test created below root, then the fixture. */
static void finish(struct fixture *fixture, const char *const *files, size_t count) {
    writer_destroy(fixture->writer);
    CHECK(catalog_destroy(fixture->catalog) == TL_OK);
    config_destroy(fixture->config);
    for (size_t i = 0; i < count; i++)
        CHECK(unlink(files[i]) == 0);
    CHECK(rmdir(fixture->sub) == 0 && rmdir(fixture->root) == 0);
    CHECK(unlink(fixture->config_path) == 0 && unlink(fixture->database) == 0);
    CHECK(rmdir(fixture->base) == 0);
}
/* Every directory is watched: repair ticks find nothing to rescan, so the
 * startup scan stays the only pass. */
static void quiet_when_covered(void) {
    struct fixture fixture;
    prepare(&fixture);
    start(&fixture);
    tl_writer_stats started = wait_idle(&fixture, 1);
    pause_ms(QUIET_WINDOW_MS);
    tl_writer_stats later = stats(&fixture);
    CHECK(later.reconciliations == started.reconciliations && full_scans(&later) == 1);
    CHECK(later.last_full_reason == WRITER_FULL_STARTUP && later.repair_scopes == 0);
    CHECK(strcmp(writer_full_reason_name(later.last_full_reason), "startup") == 0);
    finish(&fixture, NULL, 0);
}
/* With one watch slot, sub is unwatched: it joins the repair set, and a file
 * created there is found by a scoped repair, not a full scan. */
static void unwatched_subtree(void) {
    struct fixture fixture;
    prepare(&fixture);
    fixture.options.watch_capacity = 1;
    start(&fixture);
    tl_writer_stats started = wait_idle(&fixture, 1);
    CHECK(started.repair_scopes == 1 && started.watch_degraded);
    char late[256];
    CHECK(snprintf(late, sizeof(late), "%s/late.txt", fixture.sub) > 0);
    touch(late);
    wait_path(&fixture, late);
    tl_writer_stats repaired = stats(&fixture);
    CHECK(full_scans(&repaired) == 1 && repaired.scoped_reconciliations >= 1);
    CHECK(repaired.last_full_reason == WRITER_FULL_STARTUP);
    const char *files[] = {late};
    finish(&fixture, files, 1);
}
static void backstop(void) {
    struct fixture fixture;
    prepare(&fixture);
    fixture.options.repair_ms = 200;
    start(&fixture);
    wait_reason(&fixture, WRITER_FULL_BACKSTOP);
    tl_writer_stats current = stats(&fixture);
    CHECK(full_scans(&current) >= 2);
    finish(&fixture, NULL, 0);
}
/* A socket pair stands in for the mount table's poll: out-of-band data makes
 * the writer's end poll POLLPRI, as a mount-table change does. The table the
 * writer then reads is a file the test rewrites (mounts_path). */
struct mount_signal {
    int writer_end, test_end;
};
static int open_test_mounts(void *context) {
    const struct mount_signal *signal = context;
    return dup(signal->writer_end);
}
/* A table holding the root filesystem plus, optionally, one more mount. */
static void write_table(const char *path, const char *mount_point) {
    FILE *file = fopen(path, "w");
    CHECK(file != NULL && fprintf(file, "1 0 252:1 / / rw - ext4 /dev/root rw\n") > 0);
    if (mount_point != NULL)
        CHECK(fprintf(file, "40 1 8:1 / %s rw - ext4 /dev/sdz1 rw\n", mount_point) > 0);
    CHECK(fclose(file) == 0);
}
/* Signal a change and wait until the writer examined the table; the pending
 * out-of-band byte keeps POLLPRI set until the test reads it back. */
static void signal_mounts(const struct fixture *fixture, const struct mount_signal *signal,
                          uint64_t seen) {
    CHECK(send(signal->test_end, "m", 1, MSG_OOB) == 1);
    for (size_t i = 0; i < WAIT_STEPS && stats(fixture).mount_changes <= seen; i++)
        pause_ms(1);
    CHECK(stats(fixture).mount_changes > seen);
    char byte = 0;
    CHECK(recv(signal->writer_end, &byte, 1, MSG_OOB) == 1 && byte == 'm');
}
/* M7: only a mount at, below or above a root causes a full scan. A backup
 * disk mounted elsewhere (the hourly Timeshift check on a stock desktop) is
 * seen and ignored, one mounted inside the root is scanned, and a table that
 * cannot be read counts as relevant. Before, every change scanned. */
static void mount_change(void) {
    struct fixture fixture;
    prepare(&fixture);
    char table[160], root[PATH_MAX], inside[PATH_MAX + 8];
    CHECK(snprintf(table, sizeof(table), "%s/mountinfo", fixture.base) > 0);
    CHECK(realpath(fixture.root, root) != NULL);
    CHECK(snprintf(inside, sizeof(inside), "%s/usb", root) > 0);
    write_table(table, NULL);
    int ends[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, ends) == 0);
    struct mount_signal signal = {ends[0], ends[1]};
    fixture.options.open_mounts = open_test_mounts;
    fixture.options.mounts_context = &signal;
    fixture.options.mounts_path = table;
    fixture.options.rescan_ms = 3600000;
    start(&fixture);
    tl_writer_stats started = wait_idle(&fixture, 1);
    write_table(table, "/run/timeshift/1/backup");
    signal_mounts(&fixture, &signal, started.mount_changes);
    pause_ms(QUIET_WINDOW_MS);
    tl_writer_stats ignored = stats(&fixture);
    CHECK(full_scans(&ignored) == 1 && ignored.last_full_reason == WRITER_FULL_STARTUP);
    write_table(table, inside);
    signal_mounts(&fixture, &signal, ignored.mount_changes);
    wait_reason(&fixture, WRITER_FULL_MOUNT);
    tl_writer_stats mounted = wait_idle(&fixture, ignored.reconciliations + 1);
    CHECK(full_scans(&mounted) == 2);
    CHECK(unlink(table) == 0);
    signal_mounts(&fixture, &signal, mounted.mount_changes);
    tl_writer_stats unreadable = wait_idle(&fixture, mounted.reconciliations + 1);
    CHECK(full_scans(&unreadable) >= 3 && unreadable.last_full_reason == WRITER_FULL_MOUNT);
    CHECK(close(ends[0]) == 0 && close(ends[1]) == 0);
    finish(&fixture, NULL, 0);
}
static void count_release(void *context) {
    atomic_fetch_add((atomic_size_t *)context, 1);
}
static void wait_releases(atomic_size_t *releases, size_t wanted) {
    for (size_t i = 0; i < WAIT_STEPS && atomic_load(releases) < wanted; i++)
        pause_ms(1);
    CHECK(atomic_load(releases) == wanted);
}
/* A retired base engine or a full scan's batch is released once; a scoped
 * pass publishing a delta shares the base and releases nothing. On a fresh
 * database the startup scan registers the root, so it rebuilds fully and
 * retires the base loaded at creation (one release for both). With a delta
 * bound of two, three new files compact in the pass that holds the third
 * (the last), so a later single file is a plain delta again. */
static void memory_release(void) {
    struct fixture fixture;
    prepare(&fixture);
    atomic_size_t releases;
    atomic_init(&releases, 0);
    fixture.options.rescan_ms = 3600000;
    fixture.options.delta_entries = 2;
    fixture.options.release_memory = count_release;
    fixture.options.release_context = &releases;
    start(&fixture);
    tl_writer_stats started = wait_idle(&fixture, 1);
    wait_releases(&releases, 1);
    char files[4][256];
    for (int i = 0; i < 3; i++) {
        CHECK(snprintf(files[i], sizeof(files[i]), "%s/burst-%d.txt", fixture.root, i) > 0);
        touch(files[i]);
    }
    for (int i = 0; i < 3; i++)
        wait_path(&fixture, files[i]);
    wait_releases(&releases, 2);
    tl_writer_stats compacted = stats(&fixture);
    CHECK(compacted.full_builds == started.full_builds + 1);
    CHECK(snprintf(files[3], sizeof(files[3]), "%s/single.txt", fixture.root) > 0);
    touch(files[3]);
    wait_path(&fixture, files[3]);
    pause_ms(1500); /* past the one-second release spacing */
    tl_writer_stats delta = stats(&fixture);
    CHECK(delta.delta_publications > compacted.delta_publications);
    CHECK(delta.full_builds == compacted.full_builds && atomic_load(&releases) == 2);
    /* A no-change full scan publishes nothing but frees its batch. */
    CHECK(writer_reconcile(fixture.writer) == TL_OK);
    wait_reason(&fixture, WRITER_FULL_RECONCILE);
    wait_releases(&releases, 3);
    const char *created[] = {files[0], files[1], files[2], files[3]};
    finish(&fixture, created, 4);
}
/* Removing more base rows than the live delta bound publishes promptly as a
 * delta, without a full build or a base-memory release. */
static void incremental_deletion(void) {
    struct fixture fixture;
    prepare(&fixture);
    atomic_size_t releases;
    atomic_init(&releases, 0);
    fixture.options.rescan_ms = 3600000;
    fixture.options.delta_entries = 2;
    fixture.options.release_memory = count_release;
    fixture.options.release_context = &releases;
    char files[3][256];
    for (int i = 0; i < 3; i++) {
        CHECK(snprintf(files[i], sizeof(files[i]), "%s/remove-%d.txt", fixture.root, i) > 0);
        touch(files[i]);
    }
    start(&fixture);
    tl_writer_stats started = wait_idle(&fixture, 1);
    wait_releases(&releases, 1);
    for (size_t i = 0; i < 3; i++)
        CHECK(unlink(files[i]) == 0);
    for (size_t i = 0; i < WAIT_STEPS && has_path(fixture.catalog, files[2]); i++)
        pause_ms(1);
    for (size_t i = 0; i < 3; i++)
        CHECK(!has_path(fixture.catalog, files[i]));
    tl_writer_stats deleted = wait_idle(&fixture, started.reconciliations + 1);
    CHECK(deleted.delta_publications > started.delta_publications);
    CHECK(deleted.full_builds == started.full_builds && full_scans(&deleted) == 1);
    pause_ms(1200);
    CHECK(atomic_load(&releases) == 1);
    tl_catalog_stats current;
    CHECK(catalog_stats(fixture.catalog, &current) == TL_OK && current.entries == 2);
    finish(&fixture, NULL, 0);
}
void test_writer_repair(void) {
    quiet_when_covered();
    unwatched_subtree();
    backstop();
    mount_change();
    memory_release();
    incremental_deletion();
}
