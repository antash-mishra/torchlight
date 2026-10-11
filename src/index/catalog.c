/* Immutable resident catalog ownership. A snapshot is a base lexical engine,
 * shared and reference-counted across the snapshots derived from it, plus an
 * optional small delta engine of entries changed since the base and a
 * tombstone bitmap of base positions they replace or remove. The lifecycle
 * mutex protects pinning, publication, retirement and workspace leases;
 * queries use exclusive preallocated workspaces. */
#include "torchlight/catalog.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Live-entry map entries above this bit index the delta engine. */
#define CATALOG_DELTA_BIT UINT32_C(0x80000000)
/* A boosted id that is not live in the leased snapshot. */
#define CATALOG_ABSENT UINT32_MAX

/* A base engine and its reader workspaces. The workspaces outlive individual
 * snapshots, so their word caches stay warm across small updates. */
struct base_workspace {
    tl_lexical_workspace *workspace;
    bool leased;
};
struct catalog_base {
    tl_lexical *engine;
    uint64_t catalog_gen; /* when the base engine was built */
    struct base_workspace *workspaces;
    size_t capacity;
    atomic_size_t references;
};
struct tl_catalog_reader {
    tl_catalog_snapshot *snapshot;
    tl_lexical_workspace *delta_workspace; /* NULL without a delta */
    size_t base_workspace;                 /* leased base workspace while leased */
    tl_result *merge;                      /* 2 * LEXICAL_MAX_RESULTS, delta snapshots only */
    /* Personal boosts: each boosted id's segment position (base position,
     * delta position with CATALOG_DELTA_BIT, or CATALOG_ABSENT) for
     * boost_key, and per-query lists (base from the front, delta from the
     * back), each LEXICAL_MAX_BOOSTED long. */
    uint32_t *boost_positions;
    tl_lexical_boost *boost_lists;
    uint64_t boost_key;
    size_t boost_mapped;
    bool boost_ready, leased;
};
struct tl_catalog_snapshot {
    struct catalog_base *base;
    tl_lexical *delta;
    uint64_t *tombstones; /* bit per base position, NULL without tombstones */
    size_t tombstone_count;
    /* Live entries in ascending id order (base position, or delta position
     * with CATALOG_DELTA_BIT), NULL for a plain base snapshot. */
    uint32_t *live;
    size_t live_count;
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
/* ---- bases ---------------------------------------------------------------- */
/* Drop one reference; returns whether that freed the base. */
static bool base_release(struct catalog_base *base) {
    if (base == NULL || atomic_fetch_sub(&base->references, 1) != 1)
        return false;
    for (size_t i = 0; i < base->capacity; i++)
        lexical_workspace_destroy(base->workspaces[i].workspace);
    free(base->workspaces);
    lexical_destroy(base->engine);
    free(base);
    return true;
}
static tl_status base_create(tl_lexical *engine, size_t capacity, struct catalog_base **out) {
    struct catalog_base *base = calloc(1, sizeof(*base));
    if (base == NULL)
        return TL_NOMEM;
    atomic_init(&base->references, 1);
    base->workspaces = calloc(capacity, sizeof(struct base_workspace));
    tl_status status = base->workspaces == NULL ? TL_NOMEM : TL_OK;
    for (size_t i = 0; i < capacity && status == TL_OK; i++) {
        status = lexical_workspace_create(engine, &base->workspaces[i].workspace);
        base->capacity = i + 1;
    }
    if (status != TL_OK) {
        bool freed = base_release(base); /* engine stays with the caller */
        (void)freed;
        return status;
    }
    base->engine = engine;
    *out = base;
    return TL_OK;
}
/* ---- snapshots ---------------------------------------------------------------- */
/* Free snapshot; returns whether its base went with it (last reference). */
static bool snapshot_free(tl_catalog_snapshot *snapshot) {
    if (snapshot == NULL)
        return false;
    for (size_t i = 0; i < snapshot->reader_capacity; i++) {
        lexical_workspace_destroy(snapshot->readers[i].delta_workspace);
        free(snapshot->readers[i].merge);
        free(snapshot->readers[i].boost_positions);
        free(snapshot->readers[i].boost_lists);
    }
    free(snapshot->readers);
    lexical_destroy(snapshot->delta);
    free(snapshot->tombstones);
    free(snapshot->live);
    bool freed = base_release(snapshot->base);
    free(snapshot);
    return freed;
}
void catalog_snapshot_destroy(tl_catalog_snapshot *snapshot) {
    bool freed = snapshot_free(snapshot);
    (void)freed;
}
/* Reader slots; a delta snapshot gives each its own delta workspace and the
 * merge buffers that combine both segments without allocating per query. */
static tl_status create_readers(tl_catalog_snapshot *snapshot, size_t capacity) {
    snapshot->readers = calloc(capacity, sizeof(*snapshot->readers));
    if (snapshot->readers == NULL)
        return TL_NOMEM;
    tl_status status = TL_OK;
    for (size_t i = 0; i < capacity && status == TL_OK; i++) {
        tl_catalog_reader *reader = &snapshot->readers[i];
        reader->snapshot = snapshot;
        snapshot->reader_capacity = i + 1;
        reader->boost_positions = malloc(LEXICAL_MAX_BOOSTED * sizeof(uint32_t));
        reader->boost_lists = malloc(LEXICAL_MAX_BOOSTED * sizeof(tl_lexical_boost));
        if (reader->boost_positions == NULL || reader->boost_lists == NULL)
            status = TL_NOMEM;
        if (snapshot->delta == NULL || status != TL_OK)
            continue;
        status = lexical_workspace_create(snapshot->delta, &reader->delta_workspace);
        reader->merge = malloc(2 * LEXICAL_MAX_RESULTS * sizeof(tl_result));
        if (status == TL_OK && reader->merge == NULL)
            status = TL_NOMEM;
    }
    return status;
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
    tl_status status = base_create(*engine, reader_capacity, &snapshot->base);
    if (status == TL_OK)
        status = create_readers(snapshot, reader_capacity);
    if (status != TL_OK) {
        if (snapshot->base != NULL)
            snapshot->base->engine = NULL; /* ownership returns to the caller */
        catalog_snapshot_destroy(snapshot);
        return status;
    }
    snapshot->catalog_gen = catalog_gen;
    snapshot->base->catalog_gen = catalog_gen;
    *engine = NULL;
    *out = snapshot;
    return TL_OK;
}
static bool tombstoned(const tl_catalog_snapshot *snapshot, size_t position) {
    return snapshot->tombstones != NULL &&
           ((snapshot->tombstones[position / 64] >> (position % 64)) & 1U);
}
/* Copy the source's tombstones and add the base positions of retired ids. */
static tl_status build_tombstones(tl_catalog_snapshot *snapshot, const tl_catalog_snapshot *source,
                                  const uint64_t *retired, size_t retired_count) {
    const tl_lexical *engine = snapshot->base->engine;
    size_t words = lexical_count(engine) / 64 + 1;
    snapshot->tombstones = calloc(words, sizeof(uint64_t));
    if (snapshot->tombstones == NULL)
        return TL_NOMEM;
    if (source->tombstones != NULL)
        memcpy(snapshot->tombstones, source->tombstones, words * sizeof(uint64_t));
    snapshot->tombstone_count = source->tombstone_count;
    for (size_t i = 0; i < retired_count; i++) {
        size_t position = 0;
        if (lexical_slot(engine, retired[i], &position) != TL_OK || tombstoned(snapshot, position))
            continue;
        snapshot->tombstones[position / 64] |= UINT64_C(1) << (position % 64);
        snapshot->tombstone_count++;
    }
    return TL_OK;
}
/* Merge live base positions and delta positions into one id-ordered map. */
static tl_status build_live(tl_catalog_snapshot *snapshot) {
    const tl_lexical *engine = snapshot->base->engine;
    size_t base = lexical_count(engine), delta = lexical_count(snapshot->delta);
    if (base >= CATALOG_DELTA_BIT || delta >= CATALOG_DELTA_BIT)
        return TL_LIMIT;
    snapshot->live = malloc((base + delta + 1) * sizeof(uint32_t));
    if (snapshot->live == NULL)
        return TL_NOMEM;
    size_t b = 0, d = 0, count = 0;
    while (b < base || d < delta) {
        uint64_t base_id = b < base ? lexical_id(engine, b) : UINT64_MAX;
        uint64_t delta_id = d < delta ? lexical_id(snapshot->delta, d) : UINT64_MAX;
        /* A delta entry supersedes a base entry with its id even untombstoned. */
        if (b < base && (tombstoned(snapshot, b) || base_id == delta_id)) {
            b++;
            continue;
        }
        bool from_base = b < base && base_id < delta_id;
        snapshot->live[count++] = from_base ? (uint32_t)b++ : (uint32_t)d++ | CATALOG_DELTA_BIT;
    }
    snapshot->live_count = count;
    return TL_OK;
}
tl_status catalog_snapshot_derive(tl_catalog_snapshot *source, tl_lexical **delta,
                                  const uint64_t *retired, size_t retired_count,
                                  uint64_t catalog_gen, size_t reader_capacity,
                                  tl_catalog_snapshot **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (source == NULL || delta == NULL || (retired == NULL && retired_count != 0) ||
        reader_capacity == 0 || reader_capacity > CATALOG_MAX_READERS)
        return TL_INVALID;
    tl_catalog_snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL)
        return TL_NOMEM;
    snapshot->base = source->base;
    atomic_fetch_add(&snapshot->base->references, 1);
    snapshot->delta = *delta;
    tl_status status = build_tombstones(snapshot, source, retired, retired_count);
    if (status == TL_OK)
        status = build_live(snapshot);
    if (status == TL_OK)
        status = create_readers(snapshot, reader_capacity);
    if (status != TL_OK) {
        snapshot->delta = NULL; /* ownership returns to the caller */
        catalog_snapshot_destroy(snapshot);
        return status;
    }
    snapshot->catalog_gen = catalog_gen;
    *delta = NULL;
    *out = snapshot;
    return TL_OK;
}
const tl_lexical *catalog_snapshot_base(const tl_catalog_snapshot *snapshot) {
    return snapshot == NULL ? NULL : snapshot->base->engine;
}
size_t catalog_snapshot_tombstones(const tl_catalog_snapshot *snapshot) {
    return snapshot == NULL ? 0 : snapshot->tombstone_count;
}
uint64_t catalog_snapshot_base_gen(const tl_catalog_snapshot *snapshot) {
    return snapshot == NULL ? 0 : snapshot->base->catalog_gen;
}
tl_status catalog_snapshot_changes(const tl_catalog_snapshot *snapshot,
                                   tl_status (*report)(void *context, uint64_t id), void *context) {
    if (snapshot == NULL || report == NULL)
        return TL_INVALID;
    tl_status status = TL_OK;
    for (size_t i = 0; i < lexical_count(snapshot->delta) && status == TL_OK; i++)
        status = report(context, lexical_id(snapshot->delta, i));
    size_t base = lexical_count(snapshot->base->engine);
    for (size_t word = 0; snapshot->tombstones != NULL && word * 64 < base && status == TL_OK;
         word++) {
        /* Skip empty bitmap words: tombstones are few. */
        for (uint64_t bits = snapshot->tombstones[word]; bits != 0 && status == TL_OK;
             bits &= bits - 1) {
            size_t position = word * 64 + (size_t)__builtin_ctzll(bits);
            status = report(context, lexical_id(snapshot->base->engine, position));
        }
    }
    return status;
}
tl_status catalog_snapshot_find(const tl_catalog_snapshot *snapshot, uint64_t id, const char **path,
                                bool *is_dir, const char **context) {
    if (snapshot == NULL || path == NULL || is_dir == NULL || context == NULL)
        return TL_INVALID;
    const tl_lexical *engine = snapshot->delta;
    size_t position = 0;
    if (engine == NULL || lexical_slot(engine, id, &position) != TL_OK) {
        engine = snapshot->base->engine;
        if (lexical_slot(engine, id, &position) != TL_OK || tombstoned(snapshot, position))
            return TL_STATE;
    }
    uint64_t found = 0;
    tl_status status = lexical_entry(engine, position, &found, path, is_dir);
    *context = status == TL_OK ? lexical_context_path(engine, position) : NULL;
    return status;
}
/* ---- publication and leases -------------------------------------------------- */
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
static tl_lexical_workspace *base_workspace(const tl_catalog_reader *reader) {
    return reader->snapshot->base->workspaces[reader->base_workspace].workspace;
}
/* Under the lifecycle lock: a free reader slot and a free base workspace. */
static tl_catalog_reader *lease(tl_catalog_snapshot *snapshot) {
    struct catalog_base *base = snapshot->base;
    size_t workspace = 0;
    while (workspace < base->capacity && base->workspaces[workspace].leased)
        workspace++;
    if (workspace == base->capacity)
        return NULL;
    for (size_t i = 0; i < snapshot->reader_capacity; i++) {
        tl_catalog_reader *reader = &snapshot->readers[i];
        if (reader->leased)
            continue;
        reader->leased = true;
        reader->base_workspace = workspace;
        base->workspaces[workspace].leased = true;
        /* Tombstones belong to this snapshot; the shared workspace takes them
         * for the duration of the lease. */
        lexical_workspace_exclude(base->workspaces[workspace].workspace, snapshot->tombstones);
        snapshot->leased++;
        return reader;
    }
    return NULL;
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
    if (snapshot != NULL)
        *out = lease(snapshot);
    if (*out != NULL) {
        catalog->readers++;
        status = TL_OK;
    }
    unlock_catalog(catalog);
    return status;
}
void catalog_release(tl_catalog_reader *reader) {
    if (reader == NULL)
        return;
    tl_catalog_snapshot *snapshot = reader->snapshot;
    tl_catalog *catalog = snapshot->owner;
    /* The lease is still exclusive: drop its boosts outside the lock.
     * Clearing a workspace's boosts cannot fail. */
    tl_status cleared = lexical_workspace_boost(base_workspace(reader), NULL, 0);
    if (reader->delta_workspace != NULL)
        cleared = lexical_workspace_boost(reader->delta_workspace, NULL, 0);
    (void)cleared;
    lock_catalog(catalog);
    reader->leased = false;
    /* The shared workspace must not keep this lease's flag or tombstones. */
    lexical_workspace_cancel(base_workspace(reader), NULL);
    lexical_workspace_exclude(base_workspace(reader), NULL);
    lexical_workspace_cancel(reader->delta_workspace, NULL);
    snapshot->base->workspaces[reader->base_workspace].leased = false;
    snapshot->leased--;
    catalog->readers--;
    unlock_catalog(catalog);
}
void catalog_reader_cancel(tl_catalog_reader *reader, const atomic_bool *flag) {
    if (reader == NULL || !reader->leased)
        return;
    lexical_workspace_cancel(base_workspace(reader), flag);
    lexical_workspace_cancel(reader->delta_workspace, flag);
}
/* ---- queries ------------------------------------------------------------------ */
/* The engines' shared total order: score, then raw path bytes, then id. */
static bool before(const tl_result *a, const tl_result *b) {
    if (a->score != b->score)
        return a->score > b->score;
    int order = strcmp(a->path, b->path);
    return order != 0 ? order < 0 : a->id < b->id;
}
/* Merge two ordered result lists into out (capacity), returning the count.
 * Each list holds its segment's best results, so the merged head is exact. */
static size_t merge_results(const tl_result *a, size_t na, const tl_result *b, size_t nb,
                            tl_result *out, size_t capacity) {
    size_t i = 0, j = 0, count = 0;
    while (count < capacity && (i < na || j < nb))
        out[count++] = j == nb || (i < na && before(&a[i], &b[j])) ? a[i++] : b[j++];
    return count;
}
/* Map boosted ids to this lease's segment positions, once per key. */
static void map_boosts(tl_catalog_reader *reader, const tl_catalog_boosts *boosts) {
    if (reader->boost_ready && reader->boost_key == boosts->key &&
        reader->boost_mapped == boosts->count)
        return;
    const tl_catalog_snapshot *snapshot = reader->snapshot;
    for (size_t i = 0; i < boosts->count; i++) {
        size_t position = 0;
        uint32_t entry = CATALOG_ABSENT;
        if (snapshot->delta != NULL &&
            lexical_slot(snapshot->delta, boosts->ids[i], &position) == TL_OK)
            entry = (uint32_t)position | CATALOG_DELTA_BIT;
        else if (lexical_slot(snapshot->base->engine, boosts->ids[i], &position) == TL_OK &&
                 !tombstoned(snapshot, position))
            entry = (uint32_t)position;
        reader->boost_positions[i] = entry;
    }
    reader->boost_key = boosts->key;
    reader->boost_mapped = boosts->count;
    reader->boost_ready = true;
}
/* Attach this query's boosts (or none) to the lease's segment workspaces. */
static tl_status attach_boosts(tl_catalog_reader *reader, const tl_catalog_boosts *boosts) {
    size_t base = 0, delta = LEXICAL_MAX_BOOSTED;
    if (boosts != NULL && boosts->count != 0) {
        if (boosts->count > LEXICAL_MAX_BOOSTED)
            return TL_LIMIT;
        if (boosts->ids == NULL || boosts->values == NULL)
            return TL_INVALID;
        map_boosts(reader, boosts);
    }
    for (size_t i = 0; boosts != NULL && i < boosts->count; i++) {
        uint32_t entry = reader->boost_positions[i];
        if (boosts->values[i] == 0 || entry == CATALOG_ABSENT)
            continue;
        tl_lexical_boost boost = {entry & ~CATALOG_DELTA_BIT, boosts->values[i]};
        if ((entry & CATALOG_DELTA_BIT) != 0)
            reader->boost_lists[--delta] = boost;
        else
            reader->boost_lists[base++] = boost;
    }
    tl_status status = lexical_workspace_boost(base_workspace(reader), reader->boost_lists, base);
    if (status == TL_OK && reader->delta_workspace != NULL)
        status = lexical_workspace_boost(reader->delta_workspace, reader->boost_lists + delta,
                                         LEXICAL_MAX_BOOSTED - delta);
    return status;
}
tl_status catalog_query(tl_catalog_reader *reader, const char *query, tl_result *results,
                        size_t capacity, size_t *out_count) {
    return catalog_query_boosted(reader, query, NULL, results, capacity, out_count);
}
tl_status catalog_query_boosted(tl_catalog_reader *reader, const char *query,
                                const tl_catalog_boosts *boosts, tl_result *results,
                                size_t capacity, size_t *out_count) {
    if (out_count == NULL)
        return TL_INVALID;
    *out_count = 0;
    if (reader == NULL || !reader->leased)
        return TL_INVALID;
    tl_status attached = attach_boosts(reader, boosts);
    if (attached != TL_OK)
        return attached;
    const tl_catalog_snapshot *snapshot = reader->snapshot;
    size_t base_count = 0, delta_count = 0;
    tl_status status = lexical_query(snapshot->base->engine, base_workspace(reader), query, results,
                                     capacity, &base_count);
    if (status != TL_OK || snapshot->delta == NULL) {
        *out_count = status == TL_OK ? base_count : 0;
        return status;
    }
    tl_result *delta = reader->merge, *merged = reader->merge + LEXICAL_MAX_RESULTS;
    status = lexical_query(snapshot->delta, reader->delta_workspace, query, delta, capacity,
                           &delta_count);
    if (status != TL_OK)
        return status;
    size_t count = merge_results(results, base_count, delta, delta_count, merged, capacity);
    memcpy(results, merged, count * sizeof(tl_result));
    *out_count = count;
    return TL_OK;
}
tl_status catalog_resolve(const tl_catalog_reader *reader, uint64_t id, const char **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (reader == NULL || !reader->leased)
        return TL_INVALID;
    const tl_catalog_snapshot *snapshot = reader->snapshot;
    if (snapshot->delta != NULL && lexical_resolve(snapshot->delta, id, out) == TL_OK)
        return TL_OK;
    size_t position = 0;
    tl_status status = lexical_slot(snapshot->base->engine, id, &position);
    if (status == TL_OK && tombstoned(snapshot, position))
        return TL_STATE;
    return status == TL_OK ? lexical_resolve(snapshot->base->engine, id, out) : status;
}
uint64_t catalog_reader_gen(const tl_catalog_reader *reader) {
    return reader == NULL ? 0 : reader->snapshot->catalog_gen;
}
bool catalog_is_dir(const tl_catalog_reader *reader, uint64_t id) {
    if (reader == NULL || !reader->leased)
        return false;
    const tl_catalog_snapshot *snapshot = reader->snapshot;
    size_t position = 0;
    if (snapshot->delta != NULL && lexical_slot(snapshot->delta, id, &position) == TL_OK)
        return lexical_is_dir(snapshot->delta, id);
    if (lexical_slot(snapshot->base->engine, id, &position) != TL_OK ||
        tombstoned(snapshot, position))
        return false;
    return lexical_is_dir(snapshot->base->engine, id);
}
/* ---- retirement and statistics ------------------------------------------------ */
size_t catalog_reclaim(tl_catalog *catalog) {
    if (catalog == NULL)
        return 0;
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
    size_t bases = 0;
    while (garbage != NULL) {
        tl_catalog_snapshot *next = garbage->next;
        bases += snapshot_free(garbage) ? 1 : 0;
        /* Detached snapshots still count toward capacity while being freed,
         * including when another publisher/reclaimer runs concurrently. */
        lock_catalog(catalog);
        catalog->snapshots--;
        unlock_catalog(catalog);
        garbage = next;
    }
    return bases;
}
static size_t live_entries(const tl_catalog_snapshot *snapshot) {
    return lexical_count(snapshot->base->engine) - snapshot->tombstone_count +
           lexical_count(snapshot->delta);
}
tl_status catalog_stats(tl_catalog *catalog, tl_catalog_stats *out) {
    if (catalog == NULL || out == NULL)
        return TL_INVALID;
    lock_catalog(catalog);
    *out = (tl_catalog_stats){.snapshots = catalog->snapshots, .readers = catalog->readers};
    if (catalog->active != NULL) {
        out->available = true;
        out->catalog_gen = catalog->active->catalog_gen;
        out->entries = live_entries(catalog->active);
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
/* ---- metadata pins -------------------------------------------------------------- */
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
    return snapshot == NULL ? 0 : live_entries(snapshot);
}
/* Resolve a live position to its segment engine and position there. */
static bool locate(const tl_catalog_snapshot *snapshot, size_t position, const tl_lexical **engine,
                   size_t *slot) {
    if (snapshot->live == NULL) {
        *engine = snapshot->base->engine;
        *slot = position;
        return position < lexical_count(*engine);
    }
    if (position >= snapshot->live_count)
        return false;
    uint32_t entry = snapshot->live[position];
    *engine = (entry & CATALOG_DELTA_BIT) != 0 ? snapshot->delta : snapshot->base->engine;
    *slot = entry & ~CATALOG_DELTA_BIT;
    return true;
}
tl_status catalog_snapshot_entry(const tl_catalog_snapshot *snapshot, size_t position, uint64_t *id,
                                 const char **path, bool *is_dir) {
    const tl_lexical *engine = NULL;
    size_t slot = 0;
    if (snapshot == NULL || !locate(snapshot, position, &engine, &slot))
        return TL_INVALID;
    return lexical_entry(engine, slot, id, path, is_dir);
}
const char *catalog_snapshot_context(const tl_catalog_snapshot *snapshot, size_t position) {
    const tl_lexical *engine = NULL;
    size_t slot = 0;
    if (snapshot == NULL || !locate(snapshot, position, &engine, &slot))
        return NULL;
    return lexical_context_path(engine, slot);
}
