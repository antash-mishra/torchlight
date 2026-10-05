/* Immutable resident catalog ownership. The lifecycle mutex protects pinning,
 * publication and retirement; queries use exclusive preallocated workspaces. */
#include "torchlight/catalog.h"
#include <pthread.h>
#include <stdlib.h>

struct tl_catalog_reader {
    tl_catalog_snapshot *snapshot;
    tl_lexical_workspace *workspace;
    bool leased;
};
struct tl_catalog_snapshot {
    tl_lexical *engine;
    tl_catalog_reader *readers;
    tl_catalog *owner;
    tl_catalog_snapshot *next;
    uint64_t catalog_gen;
    size_t reader_capacity, leased;
};
struct tl_catalog {
    pthread_mutex_t lifecycle;
    tl_catalog_snapshot *active, *retired;
    size_t snapshot_capacity, snapshots, readers;
};

/* These private normal mutexes are initialized before exposure, never copied,
 * and unlocked only by their owning thread; pthread errors cannot occur while
 * callers respect the registry lifetime contract. */
static void lock_catalog(tl_catalog *catalog) {
    int code = pthread_mutex_lock(&catalog->lifecycle);
    (void)code;
}
static void unlock_catalog(tl_catalog *catalog) {
    int code = pthread_mutex_unlock(&catalog->lifecycle);
    (void)code;
}
tl_status catalog_create(size_t snapshot_capacity, tl_catalog **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (snapshot_capacity == 0 || snapshot_capacity > CATALOG_MAX_SNAPSHOTS)
        return TL_INVALID;
    tl_catalog *catalog = calloc(1, sizeof(*catalog));
    if (catalog == NULL)
        return TL_NOMEM;
    if (pthread_mutex_init(&catalog->lifecycle, NULL) != 0) {
        free(catalog);
        return TL_IO;
    }
    catalog->snapshot_capacity = snapshot_capacity;
    *out = catalog;
    return TL_OK;
}
void catalog_snapshot_destroy(tl_catalog_snapshot *snapshot) {
    if (snapshot == NULL)
        return;
    for (size_t i = 0; i < snapshot->reader_capacity; i++)
        lexical_workspace_destroy(snapshot->readers[i].workspace);
    free(snapshot->readers);
    lexical_destroy(snapshot->engine);
    free(snapshot);
}
tl_status catalog_snapshot_create(tl_lexical **engine, uint64_t catalog_gen, size_t reader_capacity,
                                  tl_catalog_snapshot **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (engine == NULL || *engine == NULL || reader_capacity == 0 ||
        reader_capacity > CATALOG_MAX_READERS)
        return TL_INVALID;
    tl_catalog_snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL)
        return TL_NOMEM;
    snapshot->readers = calloc(reader_capacity, sizeof(*snapshot->readers));
    tl_status status = snapshot->readers == NULL ? TL_NOMEM : TL_OK;
    for (size_t i = 0; i < reader_capacity && status == TL_OK; i++) {
        snapshot->readers[i].snapshot = snapshot;
        status = lexical_workspace_create(*engine, &snapshot->readers[i].workspace);
        snapshot->reader_capacity = i + 1;
    }
    if (status != TL_OK) {
        catalog_snapshot_destroy(snapshot);
        return status;
    }
    snapshot->engine = *engine;
    snapshot->catalog_gen = catalog_gen;
    *engine = NULL;
    *out = snapshot;
    return TL_OK;
}
tl_status catalog_publish(tl_catalog *catalog, tl_catalog_snapshot **snapshot) {
    if (catalog == NULL || snapshot == NULL || *snapshot == NULL)
        return TL_INVALID;
    lock_catalog(catalog);
    tl_status status = TL_OK;
    if ((*snapshot)->owner != NULL ||
        (catalog->active != NULL && (*snapshot)->catalog_gen <= catalog->active->catalog_gen))
        status = TL_STATE;
    else if (catalog->snapshots == catalog->snapshot_capacity)
        status = TL_LIMIT;
    if (status == TL_OK) {
        if (catalog->active != NULL) {
            catalog->active->next = catalog->retired;
            catalog->retired = catalog->active;
        }
        catalog->active = *snapshot;
        catalog->active->owner = catalog;
        catalog->snapshots++;
        *snapshot = NULL;
    }
    unlock_catalog(catalog);
    return status;
}
tl_status catalog_acquire(tl_catalog *catalog, tl_catalog_reader **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (catalog == NULL)
        return TL_INVALID;
    lock_catalog(catalog);
    tl_catalog_snapshot *snapshot = catalog->active;
    tl_status status = snapshot == NULL ? TL_STATE : TL_LIMIT;
    if (snapshot != NULL) {
        for (size_t i = 0; i < snapshot->reader_capacity; i++) {
            if (snapshot->readers[i].leased)
                continue;
            snapshot->readers[i].leased = true;
            snapshot->leased++;
            catalog->readers++;
            *out = &snapshot->readers[i];
            status = TL_OK;
            break;
        }
    }
    unlock_catalog(catalog);
    return status;
}
void catalog_release(tl_catalog_reader *reader) {
    if (reader == NULL)
        return;
    tl_catalog *catalog = reader->snapshot->owner;
    lock_catalog(catalog);
    reader->leased = false;
    reader->snapshot->leased--;
    catalog->readers--;
    unlock_catalog(catalog);
}
tl_status catalog_query(tl_catalog_reader *reader, const char *query, tl_result *results,
                        size_t capacity, size_t *out_count) {
    if (out_count == NULL)
        return TL_INVALID;
    *out_count = 0;
    if (reader == NULL || !reader->leased)
        return TL_INVALID;
    return lexical_query(reader->snapshot->engine, reader->workspace, query, results, capacity,
                         out_count);
}
tl_status catalog_resolve(const tl_catalog_reader *reader, uint64_t id, const char **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (reader == NULL || !reader->leased)
        return TL_INVALID;
    return lexical_resolve(reader->snapshot->engine, id, out);
}
uint64_t catalog_reader_gen(const tl_catalog_reader *reader) {
    return reader == NULL ? 0 : reader->snapshot->catalog_gen;
}
void catalog_reclaim(tl_catalog *catalog) {
    if (catalog == NULL)
        return;
    tl_catalog_snapshot *garbage = NULL;
    lock_catalog(catalog);
    tl_catalog_snapshot **link = &catalog->retired;
    while (*link != NULL) {
        tl_catalog_snapshot *snapshot = *link;
        if (snapshot->leased != 0) {
            link = &snapshot->next;
            continue;
        }
        *link = snapshot->next;
        snapshot->next = garbage;
        garbage = snapshot;
    }
    unlock_catalog(catalog);
    while (garbage != NULL) {
        tl_catalog_snapshot *next = garbage->next;
        catalog_snapshot_destroy(garbage);
        /* Detached snapshots still count toward capacity while being freed,
         * including when another publisher/reclaimer runs concurrently. */
        lock_catalog(catalog);
        catalog->snapshots--;
        unlock_catalog(catalog);
        garbage = next;
    }
}
tl_status catalog_stats(tl_catalog *catalog, tl_catalog_stats *out) {
    if (catalog == NULL || out == NULL)
        return TL_INVALID;
    lock_catalog(catalog);
    *out = (tl_catalog_stats){.snapshots = catalog->snapshots, .readers = catalog->readers};
    if (catalog->active != NULL) {
        out->available = true;
        out->catalog_gen = catalog->active->catalog_gen;
        out->entries = lexical_count(catalog->active->engine);
    }
    unlock_catalog(catalog);
    return TL_OK;
}
tl_status catalog_destroy(tl_catalog *catalog) {
    if (catalog == NULL)
        return TL_OK;
    if (catalog->readers != 0)
        return TL_STATE;
    catalog_reclaim(catalog);
    catalog_snapshot_destroy(catalog->active);
    int code = pthread_mutex_destroy(&catalog->lifecycle);
    (void)code;
    free(catalog);
    return TL_OK;
}

bool catalog_is_dir(const tl_catalog_reader *reader, uint64_t id) {
    return reader != NULL && reader->leased && lexical_is_dir(reader->snapshot->engine, id);
}
tl_status catalog_pin(tl_catalog *catalog, tl_catalog_snapshot **out) {
    if (catalog == NULL || out == NULL)
        return TL_INVALID;
    lock_catalog(catalog);
    *out = catalog->active;
    if (*out != NULL) {
        (*out)->leased++;
        catalog->readers++;
    }
    unlock_catalog(catalog);
    return *out == NULL ? TL_STATE : TL_OK;
}
void catalog_unpin(tl_catalog_snapshot *snapshot) {
    if (snapshot == NULL)
        return;
    lock_catalog(snapshot->owner);
    snapshot->leased--;
    snapshot->owner->readers--;
    unlock_catalog(snapshot->owner);
}
uint64_t catalog_snapshot_gen(const tl_catalog_snapshot *snapshot) {
    return snapshot == NULL ? 0 : snapshot->catalog_gen;
}
size_t catalog_snapshot_count(const tl_catalog_snapshot *snapshot) {
    return snapshot == NULL ? 0 : lexical_count(snapshot->engine);
}
tl_status catalog_snapshot_entry(const tl_catalog_snapshot *snapshot, size_t position, uint64_t *id,
                                 const char **path, bool *is_dir) {
    if (snapshot == NULL)
        return TL_INVALID;
    return lexical_entry(snapshot->engine, position, id, path, is_dir);
}

const char *catalog_snapshot_context(const tl_catalog_snapshot *snapshot, size_t position) {
    return snapshot == NULL ? NULL : lexical_context_path(snapshot->engine, position);
}
