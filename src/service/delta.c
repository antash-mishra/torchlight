/* Delta bookkeeping for incremental publication. The kept list holds owned
 * copies of every entry changed since the base engine, ascending by id; each
 * batch replaces the touched ids with their committed rows (or drops deleted
 * ones) into a pending list, builds a small engine from it and derives the
 * next snapshot. Only an adopted publication replaces the kept list. */
#include "torchlight/delta.h"
#include "torchlight/vec.h"
#include <stdlib.h>
#include <string.h>
struct delta_entry {
    uint64_t id;
    char *path;
    bool is_root, is_dir;
};
struct tl_delta {
    tl_vec *kept, *pending, *rows; /* struct delta_entry */
    size_t base_entries, base_bytes, kept_bytes, pending_bytes;
    uint64_t gen, pending_gen;
};
static void clear_entries(tl_vec *entries) {
    struct delta_entry *items = vec_data(entries);
    for (size_t i = 0; i < vec_count(entries); i++)
        free(items[i].path);
    vec_clear(entries);
}
tl_status delta_create(tl_delta **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_delta *delta = calloc(1, sizeof(*delta));
    if (delta == NULL)
        return TL_NOMEM;
    tl_status status = vec_create(sizeof(struct delta_entry), &delta->kept);
    if (status == TL_OK)
        status = vec_create(sizeof(struct delta_entry), &delta->pending);
    if (status == TL_OK)
        status = vec_create(sizeof(struct delta_entry), &delta->rows);
    if (status != TL_OK) {
        delta_destroy(delta);
        return status;
    }
    *out = delta;
    return TL_OK;
}
void delta_destroy(tl_delta *delta) {
    if (delta == NULL)
        return;
    tl_vec *lists[] = {delta->kept, delta->pending, delta->rows};
    for (size_t i = 0; i < sizeof(lists) / sizeof(lists[0]); i++) {
        if (lists[i] != NULL)
            clear_entries(lists[i]);
        vec_destroy(lists[i]);
    }
    free(delta);
}
void delta_reset(tl_delta *delta, uint64_t catalog_gen, size_t entries, size_t bytes) {
    if (delta == NULL)
        return;
    clear_entries(delta->kept);
    clear_entries(delta->pending);
    delta->base_entries = entries;
    delta->base_bytes = bytes;
    delta->kept_bytes = delta->pending_bytes = 0;
    delta->gen = catalog_gen;
}
uint64_t delta_gen(const tl_delta *delta) {
    return delta == NULL ? 0 : delta->gen;
}
static tl_status append_entry(tl_vec *entries, uint64_t id, const char *path, bool is_root,
                              bool is_dir) {
    struct delta_entry entry = {id, strdup(path), is_root, is_dir};
    if (entry.path == NULL)
        return TL_NOMEM;
    tl_status status = vec_append(entries, &entry);
    if (status != TL_OK)
        free(entry.path);
    return status;
}
static tl_status collect_row(void *context, const tl_store_entry *entry) {
    return append_entry(context, entry->id, entry->path, entry->is_root, entry->is_dir);
}
static bool touched(const uint64_t *ids, size_t count, uint64_t id) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (ids[middle] < id)
            low = middle + 1;
        else
            high = middle;
    }
    return low < count && ids[low] == id;
}
/* pending = kept entries the batch did not touch, plus the batch's rows, in
 * ascending id order (both inputs are sorted). */
static tl_status merge_pending(tl_delta *delta, const uint64_t *ids, size_t count) {
    const struct delta_entry *kept = vec_const_data(delta->kept);
    const struct delta_entry *rows = vec_const_data(delta->rows);
    size_t k = 0, r = 0, kept_count = vec_count(delta->kept), row_count = vec_count(delta->rows);
    tl_status status = TL_OK;
    delta->pending_bytes = 0;
    while (status == TL_OK && (k < kept_count || r < row_count)) {
        if (k < kept_count && touched(ids, count, kept[k].id)) {
            k++;
            continue;
        }
        bool row = r < row_count && (k == kept_count || rows[r].id < kept[k].id);
        const struct delta_entry *next = row ? &rows[r++] : &kept[k++];
        status = append_entry(delta->pending, next->id, next->path, next->is_root, next->is_dir);
        delta->pending_bytes += strlen(next->path) + 1;
    }
    return status;
}
/* Conservative bounds: removals since the source are not credited, so a
 * prepared snapshot never exceeds what a full rebuild would accept. */
static bool within_limits(const tl_delta *delta, const tl_catalog_snapshot *source,
                          const tl_delta_limits *limits) {
    size_t pending = vec_count(delta->pending);
    size_t bound = delta->base_entries / (limits->divisor == 0 ? 1 : limits->divisor);
    if (bound < limits->min_entries)
        bound = limits->min_entries;
    size_t live = delta->base_entries - catalog_snapshot_tombstones(source);
    return pending <= bound && live <= limits->max_entries &&
           pending <= limits->max_entries - live && delta->base_bytes <= limits->max_path_bytes &&
           delta->pending_bytes <= limits->max_path_bytes - delta->base_bytes;
}
static tl_status build_engine(const tl_delta *delta, const tl_catalog_snapshot *source,
                              tl_lexical **out) {
    *out = NULL;
    if (vec_count(delta->pending) == 0)
        return TL_OK; /* removals only: the snapshot is base minus tombstones */
    tl_lexical *engine = NULL;
    tl_status status = lexical_create(&engine);
    if (status == TL_OK)
        status = lexical_set_reference(engine, catalog_snapshot_base(source));
    const struct delta_entry *entries = vec_const_data(delta->pending);
    for (size_t i = 0; i < vec_count(delta->pending) && status == TL_OK; i++)
        status = lexical_add_entry(engine, entries[i].id, entries[i].path, entries[i].is_root,
                                   entries[i].is_dir);
    if (status == TL_OK)
        status = lexical_finish(engine);
    if (status != TL_OK) {
        lexical_destroy(engine);
        return status;
    }
    *out = engine;
    return TL_OK;
}
tl_status delta_prepare(tl_delta *delta, tl_store *store, tl_catalog_snapshot *source,
                        const uint64_t *ids, size_t count, const tl_delta_limits *limits,
                        tl_catalog_snapshot **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (delta == NULL || store == NULL || source == NULL || limits == NULL ||
        (ids == NULL && count != 0) || limits->max_entries == 0)
        return TL_INVALID;
    if (catalog_snapshot_gen(source) != delta->gen ||
        lexical_count(catalog_snapshot_base(source)) != delta->base_entries)
        return TL_STATE;
    clear_entries(delta->rows);
    clear_entries(delta->pending);
    tl_status status =
        store_load_ids(store, ids, count, collect_row, delta->rows, &delta->pending_gen);
    if (status == TL_OK)
        status = merge_pending(delta, ids, count);
    if (status == TL_OK && !within_limits(delta, source, limits))
        status = TL_LIMIT;
    tl_lexical *engine = NULL;
    if (status == TL_OK)
        status = build_engine(delta, source, &engine);
    if (status == TL_OK)
        status = catalog_snapshot_derive(source, &engine, ids, count, delta->pending_gen,
                                         limits->readers, out);
    lexical_destroy(engine);
    clear_entries(delta->rows);
    if (status != TL_OK)
        clear_entries(delta->pending);
    return status;
}
void delta_commit(tl_delta *delta) {
    if (delta == NULL)
        return;
    tl_vec *swap = delta->kept;
    clear_entries(swap);
    delta->kept = delta->pending;
    delta->pending = swap;
    delta->kept_bytes = delta->pending_bytes;
    delta->gen = delta->pending_gen;
}
void delta_abandon(tl_delta *delta) {
    if (delta != NULL)
        clear_entries(delta->pending);
}
