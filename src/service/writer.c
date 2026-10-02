/* One worker serializes bounded reconciliation, snapshot publication and history. */
#include "torchlight/writer.h"
#include "torchlight/path.h"
#include "torchlight/store.h"
#include "torchlight/vec.h"
#include "torchlight/watch.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define WRITER_TICK_MS 50
#define WRITER_COALESCE_MS 100
#define WRITER_RETRY_MS 1000
#define SECONDS_PER_DAY 86400
struct history {
    tl_ipc_request request;
    char search_id[IPC_HISTORY_ID_BYTES + 1];
    int64_t timestamp;
};
struct rename_pair {
    char *old_path, *new_path;
    bool is_dir;
};
struct paths {
    char **items;
    bool *selected, *scanned;
    size_t count;
    bool unresolved;
};
struct tl_writer {
    tl_writer_options options;
    char *socket_path, *database;
    tl_store *store;
    tl_watch *watch, *building_watch;
    tl_crawl *crawler;
    struct paths roots;
    pthread_t thread;
    pthread_mutex_t mutex;
    bool started, requested;
    atomic_bool stop;
    tl_writer_stats stats;
    struct history history[WRITER_HISTORY_CAPACITY];
    size_t history_head, history_count;
    struct rename_pair renames[WRITER_RENAME_CAPACITY];
    size_t rename_count, applied_renames;
    bool dirty, reload;
    uint64_t due;
    uint64_t shutdown_due;
    tl_status callback_status;
    size_t scan_entries, offline_roots, unreadable_scopes;
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
struct builder {
    tl_lexical *engine;
    const tl_writer_options *options;
    size_t entries, bytes;
};
static tl_status build_entry(void *context, const tl_store_entry *entry) {
    struct builder *builder = context;
    size_t length = strlen(entry->path) + 1;
    if (builder->entries >= builder->options->max_entries ||
        length > builder->options->max_path_bytes - builder->bytes)
        return TL_LIMIT;
    tl_status status = lexical_add(builder->engine, entry->id, entry->path, entry->is_root);
    if (status == TL_OK) {
        builder->entries++;
        builder->bytes += length;
    }
    return status;
}
static tl_status make_snapshot(tl_writer *writer, bool pending, tl_catalog_snapshot **out) {
    struct builder builder = {.options = &writer->options};
    uint64_t catalog_gen = 0;
    tl_status status = lexical_create(&builder.engine);
    if (status == TL_OK)
        status = pending ? store_prepare_catalog(writer->store, build_entry, &builder, &catalog_gen)
                         : store_load_catalog(writer->store, build_entry, &builder, &catalog_gen);
    if (status == TL_OK)
        status = lexical_finish(builder.engine);
    if (status == TL_OK)
        status =
            catalog_snapshot_create(&builder.engine, catalog_gen, writer->options.readers, out);
    lexical_destroy(builder.engine);
    return status;
}
static tl_status publish(tl_writer *writer, tl_catalog_snapshot **snapshot) {
    catalog_reclaim(writer->options.catalog);
    if (writer->options.publish != NULL)
        return writer->options.publish(writer->options.publish_context, writer->options.catalog,
                                       snapshot);
    return catalog_publish(writer->options.catalog, snapshot);
}
static tl_status load_saved(tl_writer *writer) {
    tl_catalog_snapshot *snapshot = NULL;
    tl_status status = make_snapshot(writer, false, &snapshot);
    if (status == TL_OK)
        status = publish(writer, &snapshot);
    catalog_snapshot_destroy(snapshot);
    return status;
}
static void clear_renames(tl_writer *writer) {
    for (size_t i = 0; i < writer->rename_count; i++) {
        free(writer->renames[i].old_path);
        free(writer->renames[i].new_path);
    }
    writer->rename_count = 0;
    writer->applied_renames = 0;
}
static tl_status changed(void *context, const tl_watch_event *event) {
    tl_writer *writer = context;
    if (event->path != NULL && excluded(writer, event->path))
        return TL_OK;
    uint64_t due = milliseconds() + WRITER_COALESCE_MS;
    if (!writer->dirty || writer->due > due)
        writer->due = due;
    writer->dirty = true;
    if (event->overflow) {
        clear_renames(writer);
        lock_writer(writer);
        writer->stats.watch_overflows++;
        unlock_writer(writer);
        return TL_OK;
    }
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
static tl_status apply_renames(tl_writer *writer) {
    tl_status status = TL_OK;
    for (size_t i = writer->applied_renames; i < writer->rename_count && status == TL_OK; i++) {
        struct rename_pair *pair = &writer->renames[i];
        if (eligible(writer, pair->old_path, pair->is_dir) &&
            eligible(writer, pair->new_path, pair->is_dir))
            status = store_move(writer->store, pair->old_path, pair->new_path);
        if (status == TL_OK)
            writer->applied_renames = i + 1;
    }
    return status;
}
static tl_status scan_entry(void *context, const tl_crawl_entry *entry) {
    tl_writer *writer = context;
    if (atomic_load(&writer->stop))
        return writer->callback_status = TL_STATE;
    if (excluded(writer, entry->path))
        return TL_OK;
    if (++writer->scan_entries > writer->options.max_entries)
        return writer->callback_status = TL_LIMIT;
    if (entry->unreadable)
        writer->unreadable_scopes++;
    if (entry->is_dir && !entry->unreadable) {
        tl_status watched = watch_add(writer->building_watch, entry->path);
        if (watched != TL_OK && watched != TL_IO && watched != TL_LIMIT)
            return writer->callback_status = watched;
    }
    tl_status status = store_put(writer->store, entry);
    if (status != TL_OK)
        writer->callback_status = status;
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
static tl_status scan_roots(tl_writer *writer) {
    writer->scan_entries = 0;
    writer->offline_roots = 0;
    writer->unreadable_scopes = 0;
    writer->callback_status = TL_OK;
    tl_status status = apply_renames(writer);
    for (size_t i = 0; i < writer->roots.count && status == TL_OK; i++) {
        if (!writer->roots.selected[i])
            continue;
        tl_status scanned = crawl_run(writer->crawler, writer->roots.items[i], scan_entry, writer);
        if (writer->callback_status != TL_OK)
            return writer->callback_status;
        if (scanned != TL_OK && scanned != TL_IO)
            return scanned;
        writer->roots.scanned[i] = scanned == TL_OK;
        if (scanned == TL_IO) {
            writer->offline_roots++;
            status = store_keep(writer->store, writer->roots.items[i]);
        }
    }
    /* Apply cookies received during the scan before pruning old rows. The next
     * pass reconciles any directories changed after their visit. */
    if (status == TL_OK && writer->watch != NULL)
        status = drain_watch(writer);
    if (status == TL_OK)
        status = apply_renames(writer);
    if (status == TL_OK)
        status = store_roots(writer->store, forget_root, writer);
    for (size_t i = 0; i < writer->roots.count && status == TL_OK; i++)
        if (writer->roots.scanned[i])
            status = store_prune(writer->store, writer->roots.items[i]);
    return status;
}
static void update_watch_stats(tl_writer *writer, const tl_watch *watch) {
    tl_watch_stats stats = watch_stats(watch);
    lock_writer(writer);
    writer->stats.watches = stats.directories;
    writer->stats.watch_degraded = stats.unavailable != 0;
    writer->stats.watch_unavailable += stats.unavailable;
    unlock_writer(writer);
}
static tl_status reconcile(tl_writer *writer) {
    tl_catalog_stats catalog;
    catalog_reclaim(writer->options.catalog);
    tl_status status = catalog_stats(writer->options.catalog, &catalog);
    if (status != TL_OK || catalog.snapshots >= 2)
        return status == TL_OK ? TL_LIMIT : status;
    writer->dirty = false;
    writer->applied_renames = 0;
    status = prepare_paths(writer);
    if (status == TL_OK)
        status = watch_create(writer->options.watch_capacity, &writer->building_watch);
    if (status == TL_OK)
        status = store_begin(writer->store);
    bool transaction = status == TL_OK;
    if (status == TL_OK)
        status = scan_roots(writer);
    bool catalog_changed = false;
    if (status == TL_OK)
        status = store_catalog_changed(writer->store, &catalog_changed);
    if (status == TL_OK && !catalog_changed) {
        status = store_rollback(writer->store);
        if (status == TL_OK)
            transaction = false;
    }
    tl_catalog_snapshot *snapshot = NULL;
    if (status == TL_OK && catalog_changed)
        status = make_snapshot(writer, true, &snapshot);
    if (status == TL_OK && catalog_changed) {
        status = store_commit(writer->store);
        if (status == TL_OK)
            transaction = false;
    }
    if (transaction) {
        tl_status rollback = store_rollback(writer->store);
        if (rollback != TL_OK)
            status = rollback;
    }
    if (status == TL_OK) {
        clear_renames(writer);
        if (catalog_changed) {
            status = publish(writer, &snapshot);
            if (status != TL_OK)
                writer->reload = true;
        }
        watch_destroy(writer->watch);
        writer->watch = writer->building_watch;
        writer->building_watch = NULL;
        update_watch_stats(writer, writer->watch);
    }
    catalog_snapshot_destroy(snapshot);
    watch_destroy(writer->building_watch);
    writer->building_watch = NULL;
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
static void drain_history(tl_writer *writer) {
    struct history event;
    for (size_t i = 0; i < WRITER_HISTORY_CAPACITY && pop_history(writer, &event); i++) {
        if (atomic_load(&writer->stop) && milliseconds() >= writer->shutdown_due) {
            lock_writer(writer);
            writer->stats.history_dropped++;
            unlock_writer(writer);
            continue;
        }
        tl_status status = TL_OK;
        if (event.request.operation == IPC_QUERY)
            status =
                store_search(writer->store, event.search_id, event.request.query, event.timestamp);
        else if (event.request.operation == IPC_OPEN)
            status = store_open_event(
                writer->store, event.request.event_id, event.request.file_id,
                event.request.search_id[0] == 0 ? NULL : event.request.search_id, event.timestamp);
        else
            status = store_history_prune(writer->store, 0, true);
        if (status != TL_OK) {
            lock_writer(writer);
            writer->stats.history_failures++;
            unlock_writer(writer);
        }
    }
}
static void scan_cycle(tl_writer *writer) {
    uint64_t start = milliseconds();
    lock_writer(writer);
    writer->stats.indexing = true;
    unlock_writer(writer);
    tl_status status = writer->reload ? load_saved(writer) : reconcile(writer);
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
    writer->stats.degraded =
        status != TL_OK || writer->offline_roots != 0 || writer->unreadable_scopes != 0;
    writer->stats.recovering = writer->reload;
    writer->stats.last_scan_ms = milliseconds() - start;
    if (status == TL_OK)
        writer->stats.reconciliations++;
    unlock_writer(writer);
}
static void *worker(void *context) {
    tl_writer *writer = context;
    writer->dirty = true;
    writer->due = 0;
    uint64_t periodic = 0, retention = 0;
    while (!atomic_load(&writer->stop)) {
        lock_writer(writer);
        bool requested = writer->requested;
        writer->requested = false;
        unlock_writer(writer);
        if (requested) {
            writer->dirty = true;
            writer->due = 0;
        }
        if (writer->watch != NULL) {
            tl_status status = drain_watch(writer);
            if (status != TL_OK) {
                writer->dirty = true;
                writer->due = 0;
            }
        }
        drain_history(writer);
        uint64_t now = milliseconds();
        if ((writer->dirty && now >= writer->due) || now >= periodic ||
            (writer->reload && now >= writer->due)) {
            scan_cycle(writer);
            periodic = milliseconds() + writer->options.rescan_ms;
        }
        if (writer->options.history && now >= retention) {
            int64_t cutoff =
                (int64_t)time(NULL) - (int64_t)writer->options.history_days * SECONDS_PER_DAY;
            tl_status status = store_history_prune(writer->store, cutoff < 0 ? 0 : cutoff, false);
            if (status != TL_OK) {
                lock_writer(writer);
                writer->stats.history_failures++;
                unlock_writer(writer);
            }
            retention = now + 60000;
        }
        catalog_reclaim(writer->options.catalog);
        struct pollfd fd = {watch_descriptor(writer->watch), POLLIN, 0};
        int code = poll(&fd, 1, WRITER_TICK_MS);
        (void)code;
    }
    drain_history(writer);
    return NULL;
}
tl_status writer_create(const tl_writer_options *options, tl_writer **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (options == NULL || options->config == NULL || options->catalog == NULL ||
        options->socket_path == NULL || options->watch_capacity == 0 || options->max_entries == 0 ||
        options->max_path_bytes == 0 || options->readers == 0 ||
        options->readers > CATALOG_MAX_READERS || options->rescan_ms < 100 ||
        options->history_days == 0)
        return TL_INVALID;
    tl_writer *writer = calloc(1, sizeof(*writer));
    if (writer == NULL)
        return TL_NOMEM;
    writer->options = *options;
    atomic_init(&writer->stop, false);
    if (pthread_mutex_init(&writer->mutex, NULL) != 0) {
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
        status = load_saved(writer);
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
    if (writer->started) {
        int code = pthread_join(writer->thread, NULL);
        (void)code;
    }
    store_destroy(writer->store);
    watch_destroy(writer->watch);
    watch_destroy(writer->building_watch);
    crawl_destroy(writer->crawler);
    free_paths(&writer->roots);
    clear_renames(writer);
    free(writer->socket_path);
    free(writer->database);
    int code = pthread_mutex_destroy(&writer->mutex);
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
tl_status writer_history(tl_writer *writer, const tl_ipc_request *request, const char *search_id) {
    if (writer == NULL || request == NULL ||
        (request->operation != IPC_QUERY && request->operation != IPC_OPEN &&
         request->operation != IPC_HISTORY_CLEAR) ||
        (request->operation == IPC_QUERY &&
         (search_id == NULL || search_id[0] == 0 ||
          strnlen(search_id, IPC_HISTORY_ID_BYTES + 1) > IPC_HISTORY_ID_BYTES ||
          !json_utf8(search_id))))
        return TL_INVALID;
    if (memchr(request->query, 0, sizeof(request->query)) == NULL || !json_utf8(request->query) ||
        memchr(request->search_id, 0, sizeof(request->search_id)) == NULL ||
        !json_utf8(request->search_id) ||
        memchr(request->event_id, 0, sizeof(request->event_id)) == NULL ||
        !json_utf8(request->event_id))
        return TL_INVALID;
    if (request->operation == IPC_OPEN &&
        (request->event_id[0] == 0 || request->file_id == 0 || request->file_id > INT64_MAX))
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
    }
    unlock_writer(writer);
    return status;
}
tl_status writer_stats(tl_writer *writer, tl_writer_stats *out) {
    if (writer == NULL || out == NULL)
        return TL_INVALID;
    lock_writer(writer);
    *out = writer->stats;
    unlock_writer(writer);
    return TL_OK;
}
