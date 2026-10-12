/* Two workers: an indexing thread crawls roots, or only the directories that
 * watch events touched, into a private batch, builds engines from committed
 * rows and publishes snapshots; a persistence thread owns the SQLite write
 * connection and serializes catalog batches, history and retention.
 * Filesystem scans and index construction never run inside a write
 * transaction, so history writes wait at most for one short batch commit.
 * A healthy watcher makes periodic full scans unnecessary: only the repair
 * set (directories inotify cannot cover) is rescanned every rescan_ms, with a
 * backstop full scan every repair_ms. */
#include "torchlight/writer.h"
#include "torchlight/delta.h"
#include "torchlight/ledger.h"
#include "torchlight/mounts.h"
#include "torchlight/path.h"
#include "torchlight/store.h"
#include "torchlight/usage.h"
#include "torchlight/vec.h"
#include "torchlight/watch.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define WRITER_TICK_MS 50
#define WRITER_COALESCE_MS 100
#define WRITER_RETRY_MS 1000
#define WRITER_RETENTION_MS 60000
#define SECONDS_PER_DAY 86400
/* Directories one scoped reconciliation may rescan; beyond this (e.g. a large
 * recursive delete) a full scan is cheaper than many small ones. */
#define WRITER_SCOPE_CAPACITY 1024
/* Directories inotify cannot keep current that one rescan_ms tick rescans;
 * beyond this, coverage is poor enough that full scans are simpler. */
#define WRITER_REPAIR_CAPACITY 256
/* Smallest spacing of release_memory calls: trimming walks the heap. */
#define WRITER_RELEASE_MS 1000
#define WRITER_MOUNT_TABLE "/proc/self/mountinfo"
/* Smallest rescan_ms/repair_ms: shorter timers would scan continuously. */
#define WRITER_MIN_INTERVAL_MS 100
/* A delta segment may hold max(WRITER_DELTA_MIN, base / WRITER_DELTA_DIVISOR)
 * changed entries before a full rebuild compacts it: at 500k entries about
 * 15k, which builds in roughly a tenth of a second while queries over the
 * small engine stay cheap. Change tracking stops beyond the largest delta any
 * supported catalog could keep, so oversized batches go straight to a rebuild. */
#define WRITER_DELTA_MIN 4096
#define WRITER_DELTA_DIVISOR 32
#define WRITER_TRACKED_CHANGES 131072
struct history {
    tl_ipc_request request;
    char search_id[IPC_HISTORY_ID_BYTES + 1];
    int64_t timestamp;
};
struct rename_pair {
    char *old_path, *new_path;
    bool is_dir;
};
/* A directory to rescan: its direct children, or its whole subtree when it
 * appeared with contents no scan has seen. */
struct scope {
    char *path;
    bool recursive;
};
struct paths {
    char **items;
    bool *selected, *scanned;
    size_t count;
    bool unresolved;
};
/* One crawled entry with its owned path copy; entry.path aliases path.
 * unchanged: a scoped exact pass found it as saved, so it is only marked
 * seen (scoped prunes keep it) instead of written. */
struct batch_entry {
    char *path;
    tl_crawl_entry entry;
    bool unchanged;
};
/* A directory whose saved row an exact pass found changed. If the commit
 * replaced that row (a different directory now has the path), the store
 * retired its saved subtree, and the unchanged descendants the pass did not
 * write must be rescanned. */
struct dirty_directory {
    uint64_t id;
    char *path;
};
/* Everything one reconciliation wants persisted, prepared by the indexing
 * thread and applied by the persistence thread in one short transaction.
 * Renames before index pre were received before the crawl and apply before
 * upserts; later ones apply after upserts, before pruning. */
struct batch {
    tl_vec *entries; /* struct batch_entry */
    tl_vec *keep;    /* char *: offline roots whose saved rows stay */
    size_t rename_pre;
    /* Full batches prune every successfully scanned root; scoped batches prune
     * only the scopes whose rescans succeeded. */
    bool full;
    /* exact: compared with the persisted-state map (ADR 0040). A full exact
     * batch holds only new, changed or uncomparable entries and lists in gone
     * the saved rows its scan did not find; a scoped one also carries its
     * unchanged entries, to mark them seen. */
    bool exact;
    tl_vec *gone; /* uint64_t */
    struct scope scopes[WRITER_SCOPE_CAPACITY];
    size_t scope_count;
};
enum job_state { JOB_NONE, JOB_PENDING, JOB_DONE };
struct tl_writer {
    tl_writer_options options;
    char *socket_path, *database;
    /* store: persistence thread's write connection (creating thread until the
     * workers start). reader: indexing thread's read-only connection. */
    tl_store *store, *reader;
    /* One long-lived watcher: every scan adds into it, and a full scan
     * confirms its watches and sweeps the rest (ADR 0040). NULL until one
     * can be created; then only full scans can find changes. */
    tl_watch *watch;
    tl_crawl *crawler;
    struct paths roots;
    pthread_t thread, persistence_thread;
    pthread_mutex_t mutex;
    /* persist_wake: persistence thread sleeps here; job_done: indexing waits. */
    pthread_cond_t persist_wake, job_done;
    bool started, persistence_started, requested;
    /* stop ends both loops; the persistence thread also waits for the
     * indexing thread to exit so no batch can be posted after it leaves. */
    atomic_bool stop, indexing_exited;
    tl_writer_stats stats;
    struct history history[WRITER_HISTORY_CAPACITY];
    size_t history_head, history_count;
    struct rename_pair renames[WRITER_RENAME_CAPACITY];
    size_t rename_count;
    /* Directories touched by events since the last reconciliation; full asks
     * for a scan of every root instead and names the first cause. pass_reason
     * is the cause of the pass in progress (NONE for a scoped pass). */
    struct scope scopes[WRITER_SCOPE_CAPACITY];
    size_t scope_count;
    tl_writer_full_reason full, pass_reason;
    /* Indexing thread only. The repair set: recursive scopes inotify cannot
     * keep current (unwatched, unreadable, network/FUSE mounts), rebuilt by
     * each full scan and rescanned every rescan_ms. repair_overflow (below):
     * some did not fit. offline: roots the last full scan could not read,
     * probed for their return. */
    char *repairs[WRITER_REPAIR_CAPACITY];
    size_t repair_count;
    tl_vec *offline; /* char * */
    uint64_t repair_due, backstop_due;
    /* release_pending: a full scan's batch or a retired whole engine was
     * freed; release_memory runs at release_due. */
    uint64_t release_due;
    uint64_t counted_unavailable;
    uint64_t due;
    uint64_t shutdown_due;
    /* Mount-table descriptor polled for POLLPRI, -1 without one, and the
     * table read after the last signal (NULL: unknown, so the next signal
     * counts as relevant). Indexing thread only after start. */
    int mounts;
    tl_mounts *mount_table;
    tl_status callback_status;
    bool dirty, reload, last_scoped, repair_overflow, release_pending;
    size_t scan_entries, offline_roots, unreadable_scopes;
    /* Batch handoff, guarded by mutex. job_ids holds the committed batch's
     * touched row ids; job_complete is false when they cannot describe it. */
    struct batch *job;
    enum job_state job_state;
    tl_status job_status;
    /* usage_loaded: the persistence thread's startup usage load has ended. */
    bool job_changed, job_complete, usage_loaded;
    tl_vec *job_ids; /* uint64_t */
    tl_delta *delta;
    /* Indexing thread only (writer_create before start). The persisted-state
     * map scans compare against; NULL with options.exhaustive, unused while
     * incomplete. changed_ids: rows the current pass found changed, reloaded
     * into it after the commit; dirty_directories: the directories among
     * them. scan_writes: entries the current pass sends as new or changed. */
    tl_ledger *ledger;
    tl_vec *changed_ids;       /* uint64_t */
    tl_vec *dirty_directories; /* struct dirty_directory */
    uint64_t scan_writes;
    /* Startup usage summary built by the persistence thread, until the search
     * thread takes it (writer_take_usage). */
    tl_usage *usage;
};
static uint64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static void lock_writer(tl_writer *writer) {
    int code = pthread_mutex_lock(&writer->mutex);
    (void)code;
}
static void unlock_writer(tl_writer *writer) {
    int code = pthread_mutex_unlock(&writer->mutex);
    (void)code;
}
static void free_paths(struct paths *paths) {
    for (size_t i = 0; i < paths->count; i++)
        free(paths->items[i]);
    free(paths->items);
    free(paths->selected);
    free(paths->scanned);
    *paths = (struct paths){0};
}
static tl_status add_path(struct paths *paths, const char *value) {
    char *canonical = realpath(value, NULL);
    if (canonical == NULL && errno == ENOMEM)
        return TL_NOMEM;
    if (canonical == NULL) {
        if (value[0] != '/')
            return TL_INVALID;
        canonical = strdup(value);
        paths->unresolved = true;
    }
    if (canonical == NULL)
        return TL_NOMEM;
    paths->items[paths->count++] = canonical;
    return TL_OK;
}
static tl_status collect_paths(const tl_config *config, const char *wanted, struct paths *paths) {
    size_t capacity = config_entry_count(config) + 1;
    paths->items = calloc(capacity, sizeof(char *));
    paths->selected = calloc(capacity, sizeof(bool));
    paths->scanned = calloc(capacity, sizeof(bool));
    if (paths->items == NULL || paths->selected == NULL || paths->scanned == NULL)
        return TL_NOMEM;
    for (size_t i = 0; i < config_entry_count(config); i++) {
        const char *key = NULL, *value = NULL;
        tl_status status = config_entry(config, i, &key, &value);
        if (status != TL_OK)
            return status;
        if (strcmp(key, "root") != 0 && strcmp(key, "allow") != 0)
            return TL_INVALID;
        if (strcmp(key, wanted) == 0) {
            status = add_path(paths, value);
            if (status != TL_OK)
                return status;
        }
    }
    if (strcmp(wanted, "root") == 0 && paths->count == 0) {
        const char *home = getenv("HOME");
        return home == NULL || home[0] != '/' ? TL_INVALID : add_path(paths, home);
    }
    return TL_OK;
}
static tl_status prepare_paths(tl_writer *writer) {
    free_paths(&writer->roots);
    crawl_destroy(writer->crawler);
    writer->crawler = NULL;
    struct paths allowed = {0};
    tl_status status = collect_paths(writer->options.config, "root", &writer->roots);
    if (status == TL_OK)
        status = collect_paths(writer->options.config, "allow", &allowed);
    if (status == TL_OK)
        status = crawl_create(config_state_directory(writer->options.config),
                              (const char *const *)allowed.items, allowed.count, &writer->crawler);
    if (status == TL_OK)
        crawl_select_roots(writer->crawler, (const char *const *)writer->roots.items,
                           writer->roots.count, writer->roots.selected);
    free_paths(&allowed);
    return status;
}
static bool sidecar(const char *path, const char *base) {
    size_t length = strlen(base);
    if (strncmp(path, base, length) != 0)
        return false;
    const char *suffix = path + length;
    return suffix[0] == 0 || strcmp(suffix, "-wal") == 0 || strcmp(suffix, "-shm") == 0 ||
           strcmp(suffix, ".daemon.lock") == 0 || strcmp(suffix, ".lock") == 0;
}
static bool excluded(const tl_writer *writer, const char *path) {
    return sidecar(path, writer->database) || sidecar(path, writer->socket_path);
}
/* ledger: the persisted-state map being reloaded from the same rows, or NULL
 * when it is complete already (or a put failed: it stays incomplete). */
struct builder {
    tl_lexical *engine;
    tl_ledger *ledger;
    const tl_writer_options *options;
    size_t entries, bytes;
};
static tl_status build_entry(void *context, const tl_store_entry *entry) {
    struct builder *builder = context;
    size_t length = strlen(entry->path) + 1;
    if (builder->entries >= builder->options->max_entries ||
        length > builder->options->max_path_bytes - builder->bytes)
        return TL_LIMIT;
    tl_status status =
        lexical_add_entry(builder->engine, entry->id, entry->path, entry->is_root, entry->is_dir);
    if (status == TL_OK) {
        builder->entries++;
        builder->bytes += length;
    }
    /* The map is an optimization: failing to fill it never fails a build. */
    if (status == TL_OK && builder->ledger != NULL &&
        ledger_put(builder->ledger, entry->id, entry->path, entry->signature) != TL_OK)
        builder->ledger = NULL;
    return status;
}
/* Indexing thread: build a snapshot from one committed read view, reporting
 * its generation and size for the delta bookkeeping. An incomplete
 * persisted-state map is reloaded from the same rows at no extra SQL cost. */
static tl_status make_snapshot(tl_writer *writer, tl_catalog_snapshot **out,
                               struct builder *builder, uint64_t *catalog_gen) {
    *builder = (struct builder){.options = &writer->options};
    if (writer->ledger != NULL && !ledger_complete(writer->ledger)) {
        builder->ledger = writer->ledger;
        ledger_reset(builder->ledger);
    }
    tl_status status = lexical_create(&builder->engine);
    if (status == TL_OK)
        status = store_load_catalog(writer->reader, build_entry, builder, catalog_gen);
    if (status == TL_OK && builder->ledger != NULL)
        ledger_seal(builder->ledger);
    if (status == TL_OK)
        status = lexical_finish(builder->engine);
    if (status == TL_OK)
        status =
            catalog_snapshot_create(&builder->engine, *catalog_gen, writer->options.readers, out);
    lexical_destroy(builder->engine);
    builder->engine = NULL;
    return status;
}
/* Indexing thread: free unleased retired snapshots. A freed whole engine
 * (a retired full build) leaves enough free heap to hand back to the OS. */
static void reclaim(tl_writer *writer) {
    if (catalog_reclaim(writer->options.catalog) != 0)
        writer->release_pending = true;
}
static tl_status publish(tl_writer *writer, tl_catalog_snapshot **snapshot) {
    reclaim(writer);
    if (writer->options.publish != NULL)
        return writer->options.publish(writer->options.publish_context, writer->options.catalog,
                                       snapshot);
    return catalog_publish(writer->options.catalog, snapshot);
}
/* Indexing thread: rebuild the whole engine from the committed catalog and
 * publish it as a new base (startup, recovery and delta compaction). */
static tl_status publish_full(tl_writer *writer) {
    tl_catalog_snapshot *snapshot = NULL;
    struct builder builder;
    uint64_t catalog_gen = 0;
    tl_status status = make_snapshot(writer, &snapshot, &builder, &catalog_gen);
    if (status == TL_OK)
        status = publish(writer, &snapshot);
    if (status == TL_OK) {
        delta_reset(writer->delta, catalog_gen, builder.entries, builder.bytes);
        lock_writer(writer);
        writer->stats.full_builds++;
        unlock_writer(writer);
    }
    catalog_snapshot_destroy(snapshot);
    return status;
}
static tl_status load_saved(tl_writer *writer) {
    return publish_full(writer);
}
/* Indexing thread: build the delta snapshot for the committed batch over the
 * active view. TL_LIMIT or TL_STATE mean only a full rebuild can follow. */
static tl_status prepare_changes(tl_writer *writer, tl_catalog_snapshot **out) {
    if (!writer->job_complete)
        return TL_LIMIT;
    tl_catalog_snapshot *source = NULL;
    tl_status status = catalog_pin(writer->options.catalog, &source);
    tl_delta_limits limits = {.max_entries = writer->options.max_entries,
                              .max_path_bytes = writer->options.max_path_bytes,
                              .readers = writer->options.readers,
                              .min_entries = writer->options.delta_entries == 0
                                                 ? WRITER_DELTA_MIN
                                                 : writer->options.delta_entries,
                              .divisor = WRITER_DELTA_DIVISOR};
    if (status == TL_OK)
        status =
            delta_prepare(writer->delta, writer->reader, source, vec_const_data(writer->job_ids),
                          vec_count(writer->job_ids), &limits, out);
    catalog_unpin(source);
    return status;
}
/* Indexing thread: publish a committed batch as a delta over the current base
 * when its change set is complete, else (or when the delta outgrew its bound,
 * i.e. compaction) rebuild the whole engine. */
static tl_status publish_changes(tl_writer *writer) {
    tl_catalog_snapshot *snapshot = NULL;
    tl_status status = prepare_changes(writer, &snapshot);
    if (status == TL_LIMIT || status == TL_STATE)
        return publish_full(writer);
    if (status == TL_OK)
        status = publish(writer, &snapshot);
    if (status == TL_OK) {
        delta_commit(writer->delta);
        lock_writer(writer);
        writer->stats.delta_publications++;
        unlock_writer(writer);
    } else {
        delta_abandon(writer->delta);
    }
    catalog_snapshot_destroy(snapshot);
    return status;
}
static void clear_renames(tl_writer *writer) {
    for (size_t i = 0; i < writer->rename_count; i++) {
        free(writer->renames[i].old_path);
        free(writer->renames[i].new_path);
    }
    writer->rename_count = 0;
}
static void free_scopes(struct scope *scopes, size_t *count) {
    for (size_t i = 0; i < *count; i++)
        free(scopes[i].path);
    *count = 0;
}
/* Ask for a scan of every root; the first pending cause is the one reported
 * (last_full_reason) once that pass succeeds. */
static void request_full(tl_writer *writer, tl_writer_full_reason reason) {
    if (writer->full == WRITER_FULL_NONE)
        writer->full = reason;
}
/* Indexing thread: run a pass delay milliseconds from now, or sooner if one
 * is already due earlier. */
static void schedule(tl_writer *writer, uint64_t delay) {
    uint64_t due = milliseconds() + delay;
    if (!writer->dirty || writer->due > due)
        writer->due = due;
    writer->dirty = true;
}
/* Record directory (taking ownership of the copy) as a scope to rescan,
 * merging repeats. A full set or a failed copy asks for a full scan. */
static void add_scope(tl_writer *writer, char *directory, bool recursive) {
    if (directory == NULL) {
        request_full(writer, WRITER_FULL_SCOPES);
        return;
    }
    for (size_t i = 0; i < writer->scope_count; i++) {
        if (strcmp(writer->scopes[i].path, directory) == 0) {
            writer->scopes[i].recursive |= recursive;
            free(directory);
            return;
        }
    }
    if (writer->scope_count == WRITER_SCOPE_CAPACITY) {
        request_full(writer, WRITER_FULL_SCOPES);
        free(directory);
        return;
    }
    writer->scopes[writer->scope_count++] = (struct scope){directory, recursive};
}
/* An entry changed inside its parent directory: rescan the parent's children. */
static void add_parent_scope(tl_writer *writer, const char *path) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL)
        return;
    size_t length = slash == path ? 1 : (size_t)(slash - path);
    add_scope(writer, strndup(path, length), false);
}
/* Whether directory lies at or below a repair scope (exact: is one). */
static bool repairing(const tl_writer *writer, const char *directory, bool exact) {
    for (size_t i = 0; i < writer->repair_count; i++)
        if (exact ? strcmp(directory, writer->repairs[i]) == 0
                  : path_within(directory, writer->repairs[i]))
            return true;
    return false;
}
static tl_status changed(void *context, const tl_watch_event *event) {
    tl_writer *writer = context;
    if (event->path != NULL && excluded(writer, event->path))
        return TL_OK;
    /* Search reads names only. A directory's new permissions matter only if
     * it could not be listed or watched before (it is itself a repair scope,
     * reported through its watched parent); then it may be readable now. */
    bool listable = event->metadata && event->is_dir && repairing(writer, event->path, true);
    if (event->metadata && !listable)
        return TL_OK;
    schedule(writer, WRITER_COALESCE_MS);
    if (event->overflow) {
        clear_renames(writer);
        request_full(writer, WRITER_FULL_OVERFLOW);
        lock_writer(writer);
        writer->stats.watch_overflows++;
        unlock_writer(writer);
        return TL_OK;
    }
    if (event->path == NULL)
        return TL_OK;
    if (listable) {
        add_scope(writer, strdup(event->path), true);
        return TL_OK;
    }
    add_parent_scope(writer, event->path);
    if (event->old_path != NULL)
        add_parent_scope(writer, event->old_path);
    if (event->is_dir && event->created)
        add_scope(writer, strdup(event->path), true);
    if (event->old_path == NULL || event->path == NULL ||
        writer->rename_count == WRITER_RENAME_CAPACITY)
        return TL_OK;
    struct rename_pair pair = {strdup(event->old_path), strdup(event->path), event->is_dir};
    if (pair.old_path == NULL || pair.new_path == NULL) {
        free(pair.old_path);
        free(pair.new_path);
        return TL_NOMEM;
    }
    writer->renames[writer->rename_count++] = pair;
    return TL_OK;
}
static tl_status drain_watch(tl_writer *writer) {
    if (writer->watch == NULL)
        return TL_OK;
    if (writer->options.drain != NULL)
        return writer->options.drain(writer->options.drain_context, writer->watch, changed, writer);
    return watch_drain(writer->watch, changed, writer);
}
static bool eligible(tl_writer *writer, const char *path, bool directory) {
    if (excluded(writer, path))
        return false;
    char *parent = NULL;
    if (!directory) {
        parent = strdup(path);
        if (parent == NULL)
            return false;
        char *slash = strrchr(parent, '/');
        if (slash == parent)
            slash[1] = 0;
        else
            *slash = 0;
    }
    bool covered = false;
    for (size_t i = 0; i < writer->roots.count && !covered; i++)
        covered = crawl_covers(writer->crawler, writer->roots.items[i], directory ? path : parent);
    free(parent);
    return covered;
}
/* Indexing thread: drop pairs outside indexed scope so the persistence thread
 * applies the rest without filesystem or crawler access. */
static void filter_renames(tl_writer *writer) {
    size_t kept = 0;
    for (size_t i = 0; i < writer->rename_count; i++) {
        struct rename_pair *pair = &writer->renames[i];
        if (eligible(writer, pair->old_path, pair->is_dir) &&
            eligible(writer, pair->new_path, pair->is_dir)) {
            writer->renames[kept++] = *pair;
            continue;
        }
        free(pair->old_path);
        free(pair->new_path);
    }
    writer->rename_count = kept;
}
static void batch_free(struct batch *batch) {
    free_scopes(batch->scopes, &batch->scope_count);
    struct batch_entry *entries = vec_data(batch->entries);
    for (size_t i = 0; i < vec_count(batch->entries); i++)
        free(entries[i].path);
    char **keep = vec_data(batch->keep);
    for (size_t i = 0; i < vec_count(batch->keep); i++)
        free(keep[i]);
    vec_destroy(batch->entries);
    vec_destroy(batch->keep);
    vec_destroy(batch->gone);
    *batch = (struct batch){0};
}
static tl_status batch_init(struct batch *batch) {
    *batch = (struct batch){0};
    tl_status status = vec_create(sizeof(struct batch_entry), &batch->entries);
    if (status == TL_OK)
        status = vec_create(sizeof(char *), &batch->keep);
    if (status == TL_OK)
        status = vec_create(sizeof(uint64_t), &batch->gone);
    return status;
}
static tl_status batch_keep(struct batch *batch, const char *path) {
    char *copy = strdup(path);
    if (copy == NULL)
        return TL_NOMEM;
    tl_status status = vec_append(batch->keep, &copy);
    if (status != TL_OK)
        free(copy);
    return status;
}
/* ---- repair set ----------------------------------------------------------- */
/* Remember directory as a recursive scope inotify cannot keep current. A
 * scope inside an existing one is already covered; overflow (or a failed
 * copy) means only full scans can keep the catalog exact. */
static void add_repair(tl_writer *writer, const char *directory) {
    if (writer->repair_overflow || repairing(writer, directory, false))
        return;
    char *copy = writer->repair_count == WRITER_REPAIR_CAPACITY ? NULL : strdup(directory);
    if (copy == NULL) {
        writer->repair_overflow = true;
        return;
    }
    writer->repairs[writer->repair_count++] = copy;
}
/* An entry that could not be read: a directory is repaired itself, anything
 * else (a failed stat) through its parent, whose listing it belongs to. */
static void add_unreadable(tl_writer *writer, const tl_crawl_entry *entry) {
    if (entry->is_dir) {
        add_repair(writer, entry->path);
        return;
    }
    const char *slash = strrchr(entry->path, '/');
    char *parent =
        slash == NULL
            ? NULL
            : strndup(entry->path, slash == entry->path ? 1 : (size_t)(slash - entry->path));
    if (parent == NULL) {
        writer->repair_overflow = true;
        return;
    }
    add_repair(writer, parent);
    free(parent);
}
/* A mount point (or a walk's start) on a filesystem where other machines can
 * change files without this kernel's inotify hearing of it. */
static void check_filesystem(tl_writer *writer, const char *directory) {
    bool reliable = true;
    if (watch_reliable(directory, &reliable) == TL_OK && !reliable)
        add_repair(writer, directory);
}
/* A recursive rescan of directory re-verifies every repair scope inside it;
 * those that still fail come back as that scan reports them. */
static void settle_repairs(tl_writer *writer, const char *directory) {
    size_t kept = 0;
    for (size_t i = 0; i < writer->repair_count; i++) {
        if (path_within(writer->repairs[i], directory))
            free(writer->repairs[i]);
        else
            writer->repairs[kept++] = writer->repairs[i];
    }
    writer->repair_count = kept;
}
static void clear_repairs(tl_writer *writer) {
    for (size_t i = 0; i < writer->repair_count; i++)
        free(writer->repairs[i]);
    writer->repair_count = 0;
    writer->repair_overflow = false;
}
static void clear_offline(tl_writer *writer) {
    char **paths = vec_data(writer->offline);
    for (size_t i = 0; i < vec_count(writer->offline); i++)
        free(paths[i]);
    vec_clear(writer->offline);
}
static tl_status record_offline(tl_writer *writer, const char *root) {
    char *copy = strdup(root);
    tl_status status = copy == NULL ? TL_NOMEM : vec_append(writer->offline, &copy);
    if (status != TL_OK)
        free(copy);
    return status;
}
/* Nothing watches a missing root, so each repair tick probes whether an
 * offline root is a readable directory again (recreated, renamed back,
 * remounted without a mount-table signal). */
static bool root_returned(const tl_writer *writer) {
    char *const *paths = vec_const_data(writer->offline);
    for (size_t i = 0; i < vec_count(writer->offline); i++) {
        struct stat info;
        if (stat(paths[i], &info) == 0 && S_ISDIR(info.st_mode) &&
            access(paths[i], R_OK | X_OK) == 0)
            return true;
    }
    return false;
}
/* Indexing thread, every rescan_ms: rescan the repair set through scoped
 * passes, or everything when coverage itself is missing. */
static void schedule_repair(tl_writer *writer) {
    if (writer->watch == NULL || writer->repair_overflow || root_returned(writer)) {
        request_full(writer, WRITER_FULL_REPAIR);
        schedule(writer, 0);
        return;
    }
    for (size_t i = 0; i < writer->repair_count; i++)
        add_scope(writer, strdup(writer->repairs[i]), true);
    if (writer->repair_count != 0)
        schedule(writer, 0);
}
/* ---- scans ------------------------------------------------------------------ */
struct scan {
    tl_writer *writer;
    struct batch *batch;
};
/* Install a crawled directory's watch; directories it cannot cover go to the
 * repair set. Only allocation/argument failures stop the scan. */
static tl_status cover_directory(tl_writer *writer, const tl_crawl_entry *entry) {
    if (entry->device_boundary)
        check_filesystem(writer, entry->path);
    if (writer->watch == NULL)
        return TL_OK;
    tl_watch_identity identity = {entry->device, entry->inode};
    tl_status watched =
        watch_add(writer->watch, entry->path, entry->has_identity ? &identity : NULL);
    if (watched == TL_IO || watched == TL_LIMIT)
        add_repair(writer, entry->path);
    return watched == TL_IO || watched == TL_LIMIT ? TL_OK : watched;
}
/* ---- persisted-state map (ADR 0040) ---------------------------------------- */
static void clear_dirty(tl_writer *writer) {
    struct dirty_directory *directories = vec_data(writer->dirty_directories);
    for (size_t i = 0; i < vec_count(writer->dirty_directories); i++)
        free(directories[i].path);
    vec_clear(writer->dirty_directories);
    vec_clear(writer->changed_ids);
}
/* Remember a saved row this pass found changed, to reload it after commit. */
static tl_status note_dirty(tl_writer *writer, uint64_t id, const tl_crawl_entry *entry) {
    tl_status status = vec_append(writer->changed_ids, &id);
    if (status != TL_OK || !entry->is_dir)
        return status;
    struct dirty_directory directory = {id, strdup(entry->path)};
    status = directory.path == NULL ? TL_NOMEM : vec_append(writer->dirty_directories, &directory);
    if (status != TL_OK)
        free(directory.path);
    return status;
}
/* What an exact pass does with a crawled entry: write it (new, changed or not
 * comparable), only mark it seen (unchanged, scoped pass) or skip it entirely
 * (unchanged, full pass: store_unseen covers deletion instead). */
enum verdict { VERDICT_WRITE, VERDICT_SEEN, VERDICT_SKIP };
static tl_status compare_entry(tl_writer *writer, const struct batch *batch,
                               const tl_crawl_entry *entry, enum verdict *verdict) {
    *verdict = VERDICT_WRITE;
    if (!batch->exact)
        return TL_OK;
    uint64_t id = 0, saved = 0, signature = 0;
    if (!ledger_find(writer->ledger, entry->path, &id, &saved) ||
        !store_signature(entry, &signature))
        return TL_OK;
    if (signature != saved)
        return note_dirty(writer, id, entry);
    *verdict = batch->full ? VERDICT_SKIP : VERDICT_SEEN;
    return TL_OK;
}
static tl_status load_ledger_row(void *context, const tl_store_entry *entry) {
    return ledger_put(context, entry->id, entry->path, entry->signature);
}
/* Indexing thread: reload the whole map from the committed catalog. */
static void rebuild_ledger(tl_writer *writer) {
    uint64_t catalog_gen = 0;
    ledger_reset(writer->ledger);
    if (store_load_catalog(writer->reader, load_ledger_row, writer->ledger, &catalog_gen) == TL_OK)
        ledger_seal(writer->ledger);
}
/* Indexing thread, before a pass: whether it can compare with the map. A full
 * pass first reloads an incomplete map (a failed refresh); a scoped pass just
 * writes everything, as before ADR 0040. */
static bool prepare_ledger(tl_writer *writer, bool full) {
    clear_dirty(writer);
    if (writer->ledger == NULL)
        return false;
    if (full && !ledger_complete(writer->ledger))
        rebuild_ledger(writer);
    if (!ledger_complete(writer->ledger))
        return false;
    return !full || ledger_begin(writer->ledger) == TL_OK;
}
static int compare_ids(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return a < b ? -1 : a > b;
}
/* Sort ids ascending and drop repeats. */
static void sort_unique(tl_vec *ids) {
    uint64_t *data = vec_data(ids);
    size_t count = vec_count(ids), unique = 0;
    if (count > 1)
        qsort(data, count, sizeof(uint64_t), compare_ids);
    for (size_t i = 0; i < count; i++)
        if (unique == 0 || data[unique - 1] != data[i])
            data[unique++] = data[i];
    vec_truncate(ids, unique);
}
/* A changed directory whose row the commit replaced (the store retired its
 * saved subtree with it) gets a recursive rescan: the pass skipped its
 * unchanged descendants, so nothing wrote them back yet. */
static void rescan_replaced(tl_writer *writer) {
    const struct dirty_directory *directories = vec_const_data(writer->dirty_directories);
    for (size_t i = 0; i < vec_count(writer->dirty_directories); i++) {
        if (ledger_contains(writer->ledger, directories[i].id))
            continue;
        add_scope(writer, strdup(directories[i].path), true);
        schedule(writer, 0);
    }
}
/* Indexing thread, after an exact pass committed and was published: reload
 * every row the commit touched (job_ids) or rewrote (changed_ids), so the map
 * matches the catalog again. Runs after publication, so it never delays
 * search; the next pass is the first to need it. */
static void refresh_ledger(tl_writer *writer, bool catalog_changed) {
    if (catalog_changed && vec_append_array(writer->changed_ids, vec_const_data(writer->job_ids),
                                            vec_count(writer->job_ids)) != TL_OK) {
        ledger_invalidate(writer->ledger);
        return;
    }
    sort_unique(writer->changed_ids);
    const uint64_t *ids = vec_const_data(writer->changed_ids);
    size_t count = vec_count(writer->changed_ids);
    for (size_t i = 0; i < count; i++)
        ledger_remove(writer->ledger, ids[i]);
    uint64_t catalog_gen = 0;
    if (count != 0 && store_load_ids(writer->reader, ids, count, load_ledger_row, writer->ledger,
                                     &catalog_gen) != TL_OK)
        ledger_invalidate(writer->ledger);
    if (ledger_complete(writer->ledger))
        rescan_replaced(writer);
}
static tl_status scan_entry(void *context, const tl_crawl_entry *entry) {
    struct scan *scan = context;
    tl_writer *writer = scan->writer;
    if (atomic_load(&writer->stop))
        return writer->callback_status = TL_STATE;
    if (excluded(writer, entry->path))
        return TL_OK;
    if (++writer->scan_entries > writer->options.max_entries)
        return writer->callback_status = TL_LIMIT;
    if (entry->unreadable) {
        writer->unreadable_scopes++;
        add_unreadable(writer, entry);
    }
    if (entry->is_dir && !entry->unreadable) {
        tl_status covered = cover_directory(writer, entry);
        if (covered != TL_OK)
            return writer->callback_status = covered;
    }
    enum verdict verdict = VERDICT_WRITE;
    tl_status compared = compare_entry(writer, scan->batch, entry, &verdict);
    if (compared != TL_OK)
        return writer->callback_status = compared;
    if (verdict == VERDICT_SKIP)
        return TL_OK;
    writer->scan_writes += verdict == VERDICT_WRITE;
    struct batch_entry copy = {
        .path = strdup(entry->path), .entry = *entry, .unchanged = verdict == VERDICT_SEEN};
    if (copy.path == NULL)
        return writer->callback_status = TL_NOMEM;
    copy.entry.path = copy.path;
    tl_status status = vec_append(scan->batch->entries, &copy);
    if (status != TL_OK) {
        free(copy.path);
        writer->callback_status = status;
    }
    return status;
}
/* Indexing thread: crawl selected roots into the batch, installing watches. */
static tl_status scan_roots(tl_writer *writer, struct batch *batch) {
    writer->scan_entries = 0;
    writer->offline_roots = 0;
    writer->unreadable_scopes = 0;
    writer->callback_status = TL_OK;
    batch->full = true;
    /* This scan rebuilds the repair set and the offline roots from scratch. */
    clear_repairs(writer);
    clear_offline(writer);
    struct scan scan = {writer, batch};
    tl_status status = TL_OK;
    for (size_t i = 0; i < writer->roots.count && status == TL_OK; i++) {
        if (!writer->roots.selected[i])
            continue;
        tl_status scanned = crawl_run(writer->crawler, writer->roots.items[i], scan_entry, &scan);
        if (writer->callback_status != TL_OK)
            return writer->callback_status;
        if (scanned != TL_OK && scanned != TL_IO)
            return scanned;
        writer->roots.scanned[i] = scanned == TL_OK;
        if (scanned == TL_IO) {
            writer->offline_roots++;
            status = batch_keep(batch, writer->roots.items[i]);
            if (status == TL_OK)
                status = record_offline(writer, writer->roots.items[i]);
        }
    }
    /* Cookies received during the scan apply before pruning old rows. The next
     * pass reconciles any directories changed after their visit. */
    if (status == TL_OK && writer->watch != NULL)
        status = drain_watch(writer);
    if (status == TL_OK)
        filter_renames(writer);
    return status;
}
static void update_watch_stats(tl_writer *writer, const tl_watch *watch);
/* Whether any configured root's walk indexes directory itself. */
static bool covered(const tl_writer *writer, const char *directory) {
    for (size_t i = 0; i < writer->roots.count; i++)
        if (crawl_covers(writer->crawler, writer->roots.items[i], directory))
            return true;
    return false;
}
/* Keep scopes some root indexes and drop ones nested in a recursive scope.
 * An uncovered scope above a root means the root itself changed (removed,
 * moved, remounted): only a full scan can tell. Returns false then. */
static bool resolve_scopes(tl_writer *writer, struct batch *batch) {
    bool usable[WRITER_SCOPE_CAPACITY];
    /* Decide every scope against the unmodified set, then compact. */
    for (size_t i = 0; i < batch->scope_count; i++) {
        const char *path = batch->scopes[i].path;
        usable[i] = covered(writer, path);
        for (size_t r = 0; r < writer->roots.count && !usable[i]; r++)
            if (path_within(writer->roots.items[r], path))
                return false;
        for (size_t j = 0; j < batch->scope_count && usable[i]; j++)
            usable[i] = j == i || !batch->scopes[j].recursive ||
                        strcmp(batch->scopes[j].path, path) == 0 ||
                        !path_within(path, batch->scopes[j].path);
    }
    size_t kept = 0;
    for (size_t i = 0; i < batch->scope_count; i++) {
        if (usable[i])
            batch->scopes[kept++] = batch->scopes[i];
        else
            free(batch->scopes[i].path);
    }
    batch->scope_count = kept;
    return true;
}
/* A successful children scan proves a repair's branch absent only if no
 * reported directory (including unreadable entries) could contain it. This
 * also settles a failed child scan queued alongside its parent's deletion. */
static bool repair_branch_seen(const struct batch *batch, const char *directory,
                               const char *repair) {
    const struct batch_entry *entries = vec_const_data(batch->entries);
    for (size_t i = 0; i < vec_count(batch->entries); i++) {
        const tl_crawl_entry *entry = &entries[i].entry;
        if (strcmp(entry->path, directory) != 0 && path_within(entry->path, directory) &&
            path_within(repair, entry->path) &&
            (entry->is_dir || entry->unreadable || !entry->has_stat))
            return true;
    }
    return false;
}
/* Failed walks cannot prove deletion. Only successful containing listings
 * may discard the retries they leave behind; run after every scope so their
 * processing order cannot resurrect an already-deleted repair path. */
static void settle_missing_repairs(tl_writer *writer, const struct batch *batch) {
    size_t kept = 0;
    for (size_t i = 0; i < writer->repair_count; i++) {
        const char *repair = writer->repairs[i];
        bool missing = false;
        for (size_t j = 0; j < batch->scope_count && !missing; j++) {
            const struct scope *scope = &batch->scopes[j];
            if (!scope->recursive && strcmp(repair, scope->path) != 0 &&
                path_within(repair, scope->path))
                missing = !repair_branch_seen(batch, scope->path, repair);
        }
        if (missing)
            free(writer->repairs[i]);
        else
            writer->repairs[kept++] = writer->repairs[i];
    }
    writer->repair_count = kept;
}
/* Indexing thread: rescan only the event scopes into the batch, installing
 * watches into the live watcher. Unlistable scopes are not pruned and remain
 * repair scopes until a successful scan verifies them or their removal. */
static tl_status scan_scopes(tl_writer *writer, struct batch *batch) {
    writer->scan_entries = 0;
    writer->callback_status = TL_OK;
    batch->full = false;
    struct scan scan = {writer, batch};
    /* Root availability and unreadable-scope counts describe the last full scan. */
    size_t unreadable = writer->unreadable_scopes, listed = 0;
    for (size_t i = 0; i < batch->scope_count; i++) {
        struct scope scope = batch->scopes[i];
        if (scope.recursive)
            settle_repairs(writer, scope.path);
        tl_status scanned =
            crawl_scope(writer->crawler, scope.path, scope.recursive, scan_entry, &scan);
        if (writer->callback_status != TL_OK)
            return writer->callback_status;
        if (scanned != TL_OK && scanned != TL_IO)
            return scanned;
        if (scanned == TL_OK)
            batch->scopes[listed++] = scope;
        else {
            /* Failure before the first callback (e.g. an inaccessible parent)
             * otherwise loses the repairs settled before this walk. */
            add_repair(writer, scope.path);
            free(scope.path);
        }
    }
    batch->scope_count = listed;
    settle_missing_repairs(writer, batch);
    writer->unreadable_scopes = unreadable;
    tl_status status = drain_watch(writer);
    if (status == TL_OK)
        filter_renames(writer);
    if (writer->watch != NULL)
        update_watch_stats(writer, writer->watch);
    return status;
}
static tl_status forget_root(void *context, const char *root) {
    tl_writer *writer = context;
    for (size_t i = 0; i < writer->roots.count; i++)
        if (strcmp(root, writer->roots.items[i]) == 0)
            return TL_OK;
    return writer->roots.unresolved ? store_keep(writer->store, root)
                                    : store_forget_root(writer->store, root);
}
static tl_status apply_renames(tl_writer *writer, size_t begin, size_t end) {
    tl_status status = TL_OK;
    for (size_t i = begin; i < end && status == TL_OK; i++)
        status =
            store_move(writer->store, writer->renames[i].old_path, writer->renames[i].new_path);
    return status;
}
/* Persistence thread, inside the transaction: renames, upserts, kept scopes,
 * root bookkeeping and pruning of successfully scanned roots. The indexing
 * thread is blocked meanwhile, so its roots and renames are stable. */
static tl_status apply_entries(tl_writer *writer, const struct batch *batch) {
    tl_status status = apply_renames(writer, 0, batch->rename_pre);
    const struct batch_entry *entries = vec_const_data(batch->entries);
    for (size_t i = 0; i < vec_count(batch->entries) && status == TL_OK; i++) {
        if (atomic_load(&writer->stop))
            return TL_STATE;
        status = entries[i].unchanged ? store_mark_seen(writer->store, entries[i].path)
                                      : store_put(writer->store, &entries[i].entry);
    }
    char *const *keep = vec_const_data(batch->keep);
    for (size_t i = 0; i < vec_count(batch->keep) && status == TL_OK; i++)
        status = store_keep(writer->store, keep[i]);
    if (status == TL_OK)
        status = apply_renames(writer, batch->rename_pre, writer->rename_count);
    for (size_t i = 0; i < batch->scope_count && status == TL_OK; i++)
        status = batch->scopes[i].recursive
                     ? store_prune_tree(writer->store, batch->scopes[i].path)
                     : store_prune_children(writer->store, batch->scopes[i].path);
    if (!batch->full)
        return status;
    /* An exact scan skipped its unchanged entries, so it names the rows it
     * did not find instead: only those may be pruned or forgotten below. */
    if (status == TL_OK && batch->exact)
        status = store_unseen(writer->store, vec_const_data(batch->gone), vec_count(batch->gone));
    if (status == TL_OK)
        status = store_roots(writer->store, forget_root, writer);
    for (size_t i = 0; i < writer->roots.count && status == TL_OK; i++)
        if (writer->roots.scanned[i])
            status = store_prune(writer->store, writer->roots.items[i]);
    return status;
}
/* Persistence thread: commit metadata too, but report only name changes for
 * publication. A completely unchanged scan rolls back temporary bookkeeping. */
static tl_status apply_batch(tl_writer *writer, const struct batch *batch, bool *changed) {
    *changed = false;
    tl_status status = store_begin(writer->store);
    if (status != TL_OK)
        return status;
    status = apply_entries(writer, batch);
    bool metadata_changed = false;
    if (status == TL_OK)
        status = store_catalog_changed(writer->store, changed);
    if (status == TL_OK)
        status = store_metadata_changed(writer->store, &metadata_changed);
    if (status == TL_OK)
        status = *changed || metadata_changed ? store_commit(writer->store)
                                              : store_rollback(writer->store);
    if (status != TL_OK) {
        tl_status rollback = store_rollback(writer->store);
        (void)rollback;
    }
    return status;
}
/* Indexing thread: hand the batch to the persistence thread and wait. */
static tl_status persist(tl_writer *writer, struct batch *batch, bool *changed) {
    lock_writer(writer);
    writer->job = batch;
    writer->job_state = JOB_PENDING;
    int code = pthread_cond_signal(&writer->persist_wake);
    (void)code;
    while (writer->job_state != JOB_DONE) {
        code = pthread_cond_wait(&writer->job_done, &writer->mutex);
        (void)code;
    }
    tl_status status = writer->job_status;
    *changed = writer->job_changed;
    writer->job = NULL;
    writer->job_state = JOB_NONE;
    unlock_writer(writer);
    return status;
}
/* Publish the live watcher's counters. Its unavailable count is cumulative,
 * so only the growth since the last update is added to the total. */
static void update_watch_stats(tl_writer *writer, const tl_watch *watch) {
    tl_watch_stats stats = watch_stats(watch);
    lock_writer(writer);
    writer->stats.watches = stats.directories;
    writer->stats.watch_degraded = stats.unavailable != 0;
    writer->stats.watch_unavailable += stats.unavailable - writer->counted_unavailable;
    writer->counted_unavailable = stats.unavailable;
    unlock_writer(writer);
}
/* Before a full scan: start a scan generation in the live watcher, or create
 * the watcher when none exists yet (startup, or after instance exhaustion). */
static tl_status prepare_watch(tl_writer *writer) {
    writer->counted_unavailable = 0;
    if (writer->watch != NULL) {
        watch_begin(writer->watch);
        return TL_OK;
    }
    tl_status status =
        writer->options.create_watch == NULL
            ? watch_create(writer->options.watch_capacity, &writer->watch)
            : writer->options.create_watch(writer->options.watch_context,
                                           writer->options.watch_capacity, &writer->watch);
    if (status != TL_IO)
        return status;
    /* Instance exhaustion cannot be a prerequisite for repair. Until a later
     * full scan creates a watcher, every repair tick scans everything. */
    lock_writer(writer);
    writer->stats.watch_degraded = true;
    writer->stats.watch_unavailable++;
    unlock_writer(writer);
    return TL_OK;
}
/* Whether directory lies below a selected root this full scan could not read:
 * its watch stays, since nothing proved the directory gone. */
static bool unscanned_root(void *context, const char *directory) {
    const tl_writer *writer = context;
    for (size_t i = 0; i < writer->roots.count; i++)
        if (writer->roots.selected[i] && !writer->roots.scanned[i] &&
            path_within(directory, writer->roots.items[i]))
            return true;
    return false;
}
/* After a successful full scan: drop watches the scan did not confirm
 * (deleted, excluded or unrooted directories) and publish the counters. */
static void sweep_watch(tl_writer *writer) {
    if (writer->watch == NULL)
        return;
    size_t removed = watch_sweep(writer->watch, unscanned_root, writer);
    (void)removed;
    update_watch_stats(writer, writer->watch);
}
/* Indexing thread: scan privately, persist through the owner, then build the
 * committed view and publish it. A failed build or publication after a commit
 * sets reload so the saved catalog is republished before later batches. */
/* Move the pending event scopes into the batch, so events arriving during
 * this pass start a fresh set. Returns why every root must be scanned, or
 * WRITER_FULL_NONE. Without a watcher nothing else can find changes. */
static tl_writer_full_reason take_scopes(tl_writer *writer, struct batch *batch) {
    memcpy(batch->scopes, writer->scopes, writer->scope_count * sizeof(struct scope));
    batch->scope_count = writer->scope_count;
    writer->scope_count = 0;
    tl_writer_full_reason full = writer->full;
    if (full == WRITER_FULL_NONE && writer->watch == NULL)
        full = WRITER_FULL_REPAIR;
    writer->full = WRITER_FULL_NONE;
    return full;
}
/* Indexing thread: fill the batch from a scan of every root, or of only the
 * directories events touched when they describe every change. */
static tl_status collect(tl_writer *writer, struct batch *batch) {
    tl_status status = prepare_paths(writer);
    tl_writer_full_reason full = take_scopes(writer, batch);
    if (status != TL_OK)
        return status;
    if (full == WRITER_FULL_NONE && !resolve_scopes(writer, batch))
        full = WRITER_FULL_SCOPES;
    writer->pass_reason = full;
    writer->last_scoped = full == WRITER_FULL_NONE;
    if (!writer->last_scoped) {
        free_scopes(batch->scopes, &batch->scope_count);
        status = prepare_watch(writer);
    }
    if (status != TL_OK)
        return status;
    filter_renames(writer);
    batch->rename_pre = writer->rename_count;
    batch->exact = prepare_ledger(writer, !writer->last_scoped);
    writer->scan_writes = 0;
    status = writer->last_scoped ? scan_scopes(writer, batch) : scan_roots(writer, batch);
    if (status == TL_OK && batch->exact && batch->full)
        status = ledger_unseen(writer->ledger, batch->gone);
    /* An overflow during the scan discards every pair, including earlier ones. */
    if (batch->rename_pre > writer->rename_count)
        batch->rename_pre = writer->rename_count;
    return status;
}
static tl_status reconcile(tl_writer *writer) {
    tl_catalog_stats catalog;
    reclaim(writer);
    tl_status status = catalog_stats(writer->options.catalog, &catalog);
    if (status != TL_OK || catalog.snapshots >= 2)
        return status == TL_OK ? TL_LIMIT : status;
    writer->dirty = false;
    struct batch batch;
    status = batch_init(&batch);
    if (status == TL_OK)
        status = collect(writer, &batch);
    bool catalog_changed = false;
    if (status == TL_OK)
        status = persist(writer, &batch, &catalog_changed);
    bool refresh = status == TL_OK && batch.exact;
    if (refresh && catalog_changed && !writer->job_complete) {
        /* The touched rows are unknown; the full rebuild that publication
         * then performs refills the map instead. */
        ledger_invalidate(writer->ledger);
        refresh = false;
    }
    lock_writer(writer);
    writer->stats.last_scan_writes = writer->scan_writes;
    writer->stats.last_scan_unseen = vec_count(batch.gone);
    unlock_writer(writer);
    batch_free(&batch);
    /* A full scan's batch copied every path: tens of MB of small blocks that
     * would otherwise stay in the allocator's free lists. */
    if (!writer->last_scoped)
        writer->release_pending = true;
    if (status == TL_OK) {
        clear_renames(writer);
        if (!writer->last_scoped)
            sweep_watch(writer);
    } else {
        /* The taken scopes are gone; a full pass repairs whatever they held. */
        request_full(writer, WRITER_FULL_FAILURE);
    }
    if (status == TL_OK && catalog_changed)
        status = publish_changes(writer);
    if (status != TL_OK && catalog_changed)
        writer->reload = true;
    /* After a failed publication too: the commit is what the map mirrors. */
    if (refresh)
        refresh_ledger(writer, catalog_changed);
    return status;
}
static bool pop_history(tl_writer *writer, struct history *event) {
    lock_writer(writer);
    bool found = writer->history_count != 0;
    if (found) {
        *event = writer->history[writer->history_head];
        writer->history_head = (writer->history_head + 1) % WRITER_HISTORY_CAPACITY;
        writer->history_count--;
        writer->stats.history_pending = writer->history_count;
    }
    unlock_writer(writer);
    return found;
}
/* Persist one queued search or open. An open carries its search's query when
 * the daemon still had it, so the search row is saved together with it. */
static tl_status write_history(tl_writer *writer, const struct history *event) {
    const tl_ipc_request *request = &event->request;
    const char *search = request->search_id[0] == 0 ? NULL : request->search_id;
    tl_store_history_event record = {
        .search_id = search, .event_id = request->event_id, .timestamp = event->timestamp};
    if (request->operation == IPC_QUERY) {
        record.kind = STORE_HISTORY_SEARCH;
        record.search_id = event->search_id;
        record.query = request->query;
    } else if (request->desktop_id[0] != 0) {
        record.kind = STORE_HISTORY_DESKTOP_OPEN;
        record.desktop_id = request->desktop_id;
    } else {
        record.kind = STORE_HISTORY_OPEN;
        record.file_id = request->file_id;
    }
    if (request->operation == IPC_OPEN && search != NULL && request->query[0] != 0)
        record.query = request->query;
    return store_history_write(writer->store, &record);
}
static void count_history(tl_writer *writer, size_t written, size_t failed, size_t dropped) {
    lock_writer(writer);
    writer->stats.history_written += written;
    writer->stats.history_failures += failed;
    writer->stats.history_dropped += dropped;
    unlock_writer(writer);
}
/* Commit the open history batch, if any; a failed commit loses its events. */
static void end_batch(tl_writer *writer, bool *batch, size_t *pending) {
    if (!*batch)
        return;
    tl_status status = store_history_commit(writer->store);
    count_history(writer, status == TL_OK ? *pending : 0, status == TL_OK ? 0 : *pending, 0);
    *batch = false;
    *pending = 0;
}
/* Persistence thread: write queued events in one transaction (a clear runs
 * on its own), so a burst of opens costs one commit. Past the shutdown
 * deadline the rest is counted as dropped rather than delaying exit. */
static void drain_history(tl_writer *writer) {
    struct history event;
    bool batch = false, begin_failed = false;
    size_t pending = 0;
    for (size_t i = 0; i < WRITER_HISTORY_CAPACITY && pop_history(writer, &event); i++) {
        if (atomic_load(&writer->stop) && milliseconds() >= writer->shutdown_due) {
            count_history(writer, 0, 0, 1);
            continue;
        }
        if (event.request.operation == IPC_HISTORY_CLEAR) {
            end_batch(writer, &batch, &pending);
            tl_status cleared = store_history_prune(writer->store, 0, true);
            count_history(writer, cleared == TL_OK, cleared != TL_OK, 0);
            continue;
        }
        /* Without a batch, each event commits alone. A failed begin is not
         * retried in this drain: the database is likely locked elsewhere,
         * and each retry would add a busy timeout per event. */
        if (!batch && !begin_failed) {
            batch = store_history_begin(writer->store) == TL_OK;
            begin_failed = !batch;
        }
        tl_status status = write_history(writer, &event);
        if (status == TL_OK) {
            if (batch)
                pending++;
            else
                count_history(writer, 1, 0, 0);
            continue;
        }
        count_history(writer, 0, 1, 0);
        /* An SQL failure may have taken the whole batch with it: commit what
         * remains now (a lost batch counts its events as failed), so later
         * events are counted by a batch of their own. */
        if (status == TL_IO)
            end_batch(writer, &batch, &pending);
    }
    end_batch(writer, &batch, &pending);
}
/* Rebuild the usage summary row by row; a desktop id too long to keep is
 * skipped rather than failing the load. */
static tl_status record_open(void *context, uint64_t file_id, const char *desktop_id,
                             const char *query, int64_t timestamp) {
    tl_usage_target target = {file_id, file_id == 0 ? desktop_id : NULL};
    tl_status status = usage_record(context, target, query, timestamp);
    return status == TL_LIMIT ? TL_OK : status;
}
/* Persistence thread, before draining any history: build the usage summary
 * from retained opens and offer it to the search thread. Opens queued
 * meanwhile are not in it; the search thread folds in the ones it recorded. */
static void load_usage(tl_writer *writer) {
    tl_usage *usage = NULL;
    int64_t retention = (int64_t)writer->options.history_days * SECONDS_PER_DAY;
    int64_t cutoff = (int64_t)time(NULL) - retention;
    tl_status status = TL_OK;
    if (writer->options.history) {
        status = usage_create(retention, &usage);
        if (status == TL_OK)
            status =
                store_history_opens(writer->store, cutoff < 0 ? 0 : cutoff, record_open, usage);
        if (status != TL_OK) {
            usage_destroy(usage);
            usage = NULL;
        }
    }
    lock_writer(writer);
    writer->usage = usage;
    writer->usage_loaded = true;
    if (status != TL_OK)
        writer->stats.history_failures++;
    unlock_writer(writer);
}
static void prune_retention(tl_writer *writer) {
    int64_t cutoff = (int64_t)time(NULL) - (int64_t)writer->options.history_days * SECONDS_PER_DAY;
    tl_status status = store_history_prune(writer->store, cutoff < 0 ? 0 : cutoff, false);
    if (status != TL_OK) {
        lock_writer(writer);
        writer->stats.history_failures++;
        unlock_writer(writer);
    }
}
static void scan_cycle(tl_writer *writer) {
    uint64_t start = milliseconds();
    lock_writer(writer);
    writer->stats.indexing = true;
    unlock_writer(writer);
    writer->last_scoped = false;
    writer->pass_reason = WRITER_FULL_NONE;
    tl_status status = writer->reload ? load_saved(writer) : reconcile(writer);
    /* A completed full scan restarts the backstop interval. */
    if (status == TL_OK && writer->pass_reason != WRITER_FULL_NONE)
        writer->backstop_due = milliseconds() + writer->options.repair_ms;
    if (status == TL_OK)
        writer->reload = false;
    else {
        writer->dirty = true;
        writer->due = milliseconds() + (status == TL_LIMIT ? WRITER_COALESCE_MS : WRITER_RETRY_MS);
    }
    lock_writer(writer);
    writer->stats.indexing = false;
    writer->stats.offline_roots = writer->offline_roots;
    writer->stats.unreadable_scopes = writer->unreadable_scopes;
    writer->stats.degraded = status != TL_OK || writer->offline_roots != 0 ||
                             writer->unreadable_scopes != 0 || writer->stats.watch_degraded;
    writer->stats.recovering = writer->reload;
    writer->stats.last_scan_ms = milliseconds() - start;
    writer->stats.repair_scopes = writer->repair_count;
    if (status == TL_OK && writer->pass_reason != WRITER_FULL_NONE)
        writer->stats.last_full_reason = writer->pass_reason;
    if (status == TL_OK)
        writer->stats.reconciliations++;
    if (status == TL_OK && writer->last_scoped)
        writer->stats.scoped_reconciliations++;
    unlock_writer(writer);
}
/* Indexing thread: hand freed heap back at most once per WRITER_RELEASE_MS. */
static void release_memory(tl_writer *writer, uint64_t now) {
    if (!writer->release_pending || now < writer->release_due)
        return;
    writer->release_pending = false;
    writer->release_due = now + WRITER_RELEASE_MS;
    if (writer->options.release_memory != NULL)
        writer->options.release_memory(writer->options.release_context);
}
/* Indexing thread: periodic work. Every rescan_ms the repair set (directories
 * inotify cannot cover) is rescanned; every repair_ms without a full scan a
 * backstop full scan catches whatever inotify might still have missed. */
static void timers(tl_writer *writer, uint64_t now) {
    if (now >= writer->repair_due) {
        schedule_repair(writer);
        writer->repair_due = now + writer->options.rescan_ms;
    }
    if (writer->options.repair_ms != 0 && now >= writer->backstop_due) {
        request_full(writer, WRITER_FULL_BACKSTOP);
        schedule(writer, 0);
        writer->backstop_due = now + writer->options.repair_ms;
    }
}
static const char *mount_table_path(const tl_writer *writer) {
    return writer->options.mounts_path == NULL ? WRITER_MOUNT_TABLE : writer->options.mounts_path;
}
/* Indexing thread, after a mount-table signal. A mount at, below or above a
 * root can hide or reveal a whole tree without any inotify event, so it gets
 * a full scan; every other mount (hourly backup checks, snaps, containers,
 * removable media elsewhere) changes nothing indexed. A table that cannot be
 * read counts as relevant, and leaves the next signal relevant too. */
static void mount_changed(tl_writer *writer) {
    tl_mounts *table = NULL;
    tl_status status = mounts_read(mount_table_path(writer), &table);
    bool relevant = status != TL_OK ||
                    mounts_affect(writer->mount_table, table,
                                  (const char *const *)writer->roots.items, writer->roots.count);
    mounts_destroy(writer->mount_table);
    writer->mount_table = table;
    lock_writer(writer);
    writer->stats.mount_changes++;
    unlock_writer(writer);
    if (!relevant)
        return;
    request_full(writer, WRITER_FULL_MOUNT);
    schedule(writer, WRITER_COALESCE_MS);
}
/* Sleep until an inotify event, a mount-table change or the next tick. */
static void wait_events(tl_writer *writer) {
    struct pollfd fds[] = {{watch_descriptor(writer->watch), POLLIN, 0},
                           {writer->mounts, POLLPRI, 0}};
    if (poll(fds, sizeof(fds) / sizeof(fds[0]), WRITER_TICK_MS) <= 0)
        return;
    if ((fds[1].revents & (POLLPRI | POLLERR)) != 0)
        mount_changed(writer);
}
static void *worker(void *context) {
    tl_writer *writer = context;
    tl_thread_name("tl-index");
    uint64_t start = milliseconds();
    request_full(writer, WRITER_FULL_STARTUP);
    schedule(writer, 0);
    /* The startup scan builds the repair set; the first repair tick follows. */
    writer->repair_due = start + writer->options.rescan_ms;
    writer->backstop_due = start + writer->options.repair_ms;
    while (!atomic_load(&writer->stop)) {
        lock_writer(writer);
        bool requested = writer->requested;
        writer->requested = false;
        unlock_writer(writer);
        if (requested) {
            request_full(writer, WRITER_FULL_RECONCILE);
            schedule(writer, 0);
        }
        if (writer->watch != NULL && drain_watch(writer) != TL_OK) {
            request_full(writer, WRITER_FULL_OVERFLOW);
            schedule(writer, 0);
        }
        uint64_t now = milliseconds();
        timers(writer, now);
        if ((writer->dirty || writer->reload) && now >= writer->due)
            scan_cycle(writer);
        reclaim(writer);
        release_memory(writer, milliseconds());
        wait_events(writer);
    }
    lock_writer(writer);
    atomic_store(&writer->indexing_exited, true);
    int code = pthread_cond_broadcast(&writer->persist_wake);
    (void)code;
    unlock_writer(writer);
    return NULL;
}
/* Persistence thread, after a commit: copy the touched row ids for the
 * indexing thread (which waits meanwhile). False when they cannot describe
 * the commit: tracking overflowed, roots changed or the copy failed. */
static bool collect_changes(tl_writer *writer) {
    const uint64_t *ids = NULL;
    size_t count = 0;
    bool complete = false, roots = false;
    vec_clear(writer->job_ids);
    if (store_changes(writer->store, &ids, &count, &complete, &roots) != TL_OK || !complete ||
        roots)
        return false;
    return vec_append_array(writer->job_ids, ids, count) == TL_OK;
}
/* Under the mutex: sleep until there is a batch, history, shutdown or the
 * next retention tick (wait milliseconds away). */
static void await_work(tl_writer *writer, uint64_t wait) {
    if (wait > WRITER_RETENTION_MS)
        wait = WRITER_RETENTION_MS;
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
        return;
    deadline.tv_sec += (time_t)(wait / 1000);
    deadline.tv_nsec += (long)(wait % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    while (!atomic_load(&writer->stop) && !atomic_load(&writer->indexing_exited) &&
           writer->job_state != JOB_PENDING && writer->history_count == 0) {
        int code = pthread_cond_timedwait(&writer->persist_wake, &writer->mutex, &deadline);
        if (code == ETIMEDOUT)
            break;
    }
}
/* Persistence thread: the only SQLite writer once started. A pending batch is
 * always answered, even during shutdown, so the indexing thread can exit. */
static void *persistence_worker(void *context) {
    tl_writer *writer = context;
    tl_thread_name("tl-persist");
    uint64_t retention = 0;
    load_usage(writer);
    for (;;) {
        uint64_t now = milliseconds();
        uint64_t wait = !writer->options.history ? WRITER_RETENTION_MS
                        : retention > now        ? retention - now
                                                 : 0;
        lock_writer(writer);
        if (writer->job_state != JOB_PENDING && writer->history_count == 0)
            await_work(writer, wait);
        bool job = writer->job_state == JOB_PENDING;
        unlock_writer(writer);
        if (job) {
            bool changed = false, complete = false;
            tl_status status = apply_batch(writer, writer->job, &changed);
            if (status == TL_OK && changed)
                complete = collect_changes(writer);
            lock_writer(writer);
            writer->job_status = status;
            writer->job_changed = changed;
            writer->job_complete = complete;
            writer->job_state = JOB_DONE;
            int code = pthread_cond_broadcast(&writer->job_done);
            (void)code;
            unlock_writer(writer);
        }
        drain_history(writer);
        now = milliseconds();
        if (writer->options.history && now >= retention) {
            prune_retention(writer);
            retention = now + WRITER_RETENTION_MS;
        }
        if (atomic_load(&writer->stop) && atomic_load(&writer->indexing_exited)) {
            lock_writer(writer);
            bool pending = writer->job_state == JOB_PENDING || writer->history_count != 0;
            unlock_writer(writer);
            if (!pending)
                return NULL;
        }
    }
}
static tl_status init_sync(tl_writer *writer) {
    pthread_condattr_t attributes;
    if (pthread_mutex_init(&writer->mutex, NULL) != 0)
        return TL_IO;
    if (pthread_condattr_init(&attributes) != 0)
        return TL_IO;
    int code = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (code == 0)
        code = pthread_cond_init(&writer->persist_wake, &attributes);
    if (code == 0)
        code = pthread_cond_init(&writer->job_done, NULL);
    pthread_condattr_destroy(&attributes);
    return code == 0 ? TL_OK : TL_IO;
}
/* Without a mount table (no /proc), mount changes are found by the backstop
 * scan and by offline-root probes instead. The first table read is the one
 * later signals compare against; a failed read leaves it unknown. */
static void open_mounts(tl_writer *writer) {
    writer->mounts = writer->options.open_mounts != NULL
                         ? writer->options.open_mounts(writer->options.mounts_context)
                         : open(WRITER_MOUNT_TABLE, O_RDONLY | O_CLOEXEC);
    if (writer->mounts < 0)
        return;
    tl_status status = mounts_read(mount_table_path(writer), &writer->mount_table);
    (void)status; /* NULL table: the first signal is treated as relevant */
}
tl_status writer_create(const tl_writer_options *options, tl_writer **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (options == NULL || options->config == NULL || options->catalog == NULL ||
        options->socket_path == NULL || options->watch_capacity == 0 || options->max_entries == 0 ||
        options->max_path_bytes == 0 || options->readers == 0 ||
        options->readers > CATALOG_MAX_READERS || options->rescan_ms < WRITER_MIN_INTERVAL_MS ||
        (options->repair_ms != 0 && options->repair_ms < WRITER_MIN_INTERVAL_MS) ||
        options->history_days == 0)
        return TL_INVALID;
    tl_writer *writer = calloc(1, sizeof(*writer));
    if (writer == NULL)
        return TL_NOMEM;
    writer->options = *options;
    writer->mounts = -1;
    atomic_init(&writer->stop, false);
    atomic_init(&writer->indexing_exited, false);
    if (init_sync(writer) != TL_OK) {
        free(writer);
        return TL_IO;
    }
    writer->socket_path = strdup(options->socket_path);
    writer->database = strdup(config_database(options->config));
    writer->stats.history_enabled = options->history;
    tl_status status =
        writer->socket_path == NULL || writer->database == NULL ? TL_NOMEM : prepare_paths(writer);
    if (status == TL_OK)
        status = store_create(writer->database, &writer->store);
    if (status == TL_OK)
        status = store_track_changes(writer->store, WRITER_TRACKED_CHANGES);
    if (status == TL_OK)
        status = store_create(writer->database, &writer->reader);
    if (status == TL_OK)
        status = vec_create(sizeof(uint64_t), &writer->job_ids);
    if (status == TL_OK)
        status = vec_create(sizeof(uint64_t), &writer->changed_ids);
    if (status == TL_OK)
        status = vec_create(sizeof(struct dirty_directory), &writer->dirty_directories);
    if (status == TL_OK && !options->exhaustive)
        status = ledger_create(&writer->ledger);
    if (status == TL_OK)
        status = vec_create(sizeof(char *), &writer->offline);
    if (status == TL_OK)
        status = delta_create(&writer->delta);
    if (status == TL_OK)
        open_mounts(writer);
    if (status == TL_OK)
        status = load_saved(writer);
    if (status == TL_OK &&
        pthread_create(&writer->persistence_thread, NULL, persistence_worker, writer) != 0)
        status = TL_IO;
    if (status == TL_OK)
        writer->persistence_started = true;
    if (status == TL_OK && pthread_create(&writer->thread, NULL, worker, writer) != 0)
        status = TL_IO;
    if (status != TL_OK) {
        writer_destroy(writer);
        return status;
    }
    writer->started = true;
    *out = writer;
    return TL_OK;
}
void writer_destroy(tl_writer *writer) {
    if (writer == NULL)
        return;
    writer->shutdown_due = milliseconds() + IPC_DEADLINE_MS;
    atomic_store(&writer->stop, true);
    int code = 0;
    if (writer->started)
        code = pthread_join(writer->thread, NULL);
    (void)code;
    lock_writer(writer);
    atomic_store(&writer->indexing_exited, true);
    code = pthread_cond_broadcast(&writer->persist_wake);
    (void)code;
    unlock_writer(writer);
    if (writer->persistence_started)
        code = pthread_join(writer->persistence_thread, NULL);
    (void)code;
    store_destroy(writer->store);
    store_destroy(writer->reader);
    usage_destroy(writer->usage);
    vec_destroy(writer->job_ids);
    clear_repairs(writer);
    if (writer->offline != NULL)
        clear_offline(writer);
    vec_destroy(writer->offline);
    if (writer->mounts >= 0)
        close(writer->mounts);
    mounts_destroy(writer->mount_table);
    delta_destroy(writer->delta);
    if (writer->dirty_directories != NULL)
        clear_dirty(writer);
    vec_destroy(writer->changed_ids);
    vec_destroy(writer->dirty_directories);
    ledger_destroy(writer->ledger);
    watch_destroy(writer->watch);
    crawl_destroy(writer->crawler);
    free_paths(&writer->roots);
    clear_renames(writer);
    free_scopes(writer->scopes, &writer->scope_count);
    free(writer->socket_path);
    free(writer->database);
    code = pthread_cond_destroy(&writer->persist_wake);
    (void)code;
    code = pthread_cond_destroy(&writer->job_done);
    (void)code;
    code = pthread_mutex_destroy(&writer->mutex);
    (void)code;
    free(writer);
}
tl_status writer_reconcile(tl_writer *writer) {
    if (writer == NULL)
        return TL_INVALID;
    lock_writer(writer);
    writer->requested = true;
    unlock_writer(writer);
    return TL_OK;
}
static bool valid_history(const tl_ipc_request *request, const char *search_id) {
    if (request->operation != IPC_QUERY && request->operation != IPC_OPEN &&
        request->operation != IPC_HISTORY_CLEAR)
        return false;
    if (request->operation == IPC_QUERY &&
        (search_id == NULL || search_id[0] == 0 ||
         strnlen(search_id, IPC_HISTORY_ID_BYTES + 1) > IPC_HISTORY_ID_BYTES ||
         !json_utf8(search_id)))
        return false;
    if (memchr(request->query, 0, sizeof(request->query)) == NULL || !json_utf8(request->query) ||
        memchr(request->search_id, 0, sizeof(request->search_id)) == NULL ||
        !json_utf8(request->search_id) ||
        memchr(request->event_id, 0, sizeof(request->event_id)) == NULL ||
        !json_utf8(request->event_id))
        return false;
    if (memchr(request->desktop_id, 0, sizeof(request->desktop_id)) == NULL ||
        !json_utf8(request->desktop_id))
        return false;
    return request->operation != IPC_OPEN ||
           (request->event_id[0] != 0 && request->file_id != 0 && request->file_id <= INT64_MAX);
}
tl_status writer_history(tl_writer *writer, const tl_ipc_request *request, const char *search_id) {
    if (writer == NULL || request == NULL || !valid_history(request, search_id))
        return TL_INVALID;
    lock_writer(writer);
    tl_status status =
        writer->options.history || request->operation == IPC_HISTORY_CLEAR ? TL_OK : TL_STATE;
    if (status == TL_OK && writer->history_count == WRITER_HISTORY_CAPACITY) {
        writer->stats.history_dropped++;
        status = TL_LIMIT;
    }
    if (status == TL_OK) {
        size_t index = (writer->history_head + writer->history_count) % WRITER_HISTORY_CAPACITY;
        struct history *event = &writer->history[index];
        *event = (struct history){.request = *request, .timestamp = (int64_t)time(NULL)};
        if (request->operation == IPC_QUERY)
            memcpy(event->search_id, search_id, strlen(search_id) + 1);
        writer->history_count++;
        writer->stats.history_pending = writer->history_count;
        int code = pthread_cond_signal(&writer->persist_wake);
        (void)code;
    }
    unlock_writer(writer);
    return status;
}
bool writer_take_usage(tl_writer *writer, tl_usage **out) {
    if (out == NULL)
        return true;
    *out = NULL;
    if (writer == NULL)
        return true;
    lock_writer(writer);
    bool loaded = writer->usage_loaded;
    *out = writer->usage;
    writer->usage = NULL;
    unlock_writer(writer);
    return loaded;
}
tl_status writer_stats(tl_writer *writer, tl_writer_stats *out) {
    if (writer == NULL || out == NULL)
        return TL_INVALID;
    lock_writer(writer);
    *out = writer->stats;
    unlock_writer(writer);
    return TL_OK;
}
const char *writer_full_reason_name(tl_writer_full_reason reason) {
    switch (reason) {
    case WRITER_FULL_STARTUP:
        return "startup";
    case WRITER_FULL_RECONCILE:
        return "reconcile";
    case WRITER_FULL_OVERFLOW:
        return "overflow";
    case WRITER_FULL_SCOPES:
        return "scopes";
    case WRITER_FULL_FAILURE:
        return "failure";
    case WRITER_FULL_REPAIR:
        return "repair";
    case WRITER_FULL_MOUNT:
        return "mount";
    case WRITER_FULL_BACKSTOP:
        return "backstop";
    case WRITER_FULL_NONE:
        break;
    }
    return "none";
}
