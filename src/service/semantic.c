/* Serialized inference gives interactive jobs priority between background rows.
 * Published snapshots own copied metadata, so two phases need no long lexical
 * workspace or desktop lock. Retirement is bounded and reclaimed by the worker.
 * A small catalog update stages a derived snapshot: it shares a full base
 * snapshot, hides the base rows that changed or disappeared, and owns only the
 * changed rows, embedding those whose prepared text changed. Only ids the
 * catalog reports as possibly changed are examined. A full stage (first one,
 * model change, new catalog base, large delta) copies every row, still reusing
 * vectors whose text is unchanged. */
#include "torchlight/semantic.h"
#include "torchlight/hashmap.h"
#include "torchlight/potion.h"
#include "torchlight/rank.h"
#include "torchlight/sort.h"
#include "torchlight/store.h"
#include "torchlight/vec.h"
#include "torchlight/vector.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* BACKGROUND_REUSE rows copied from the previous snapshot cost about as much
 * as one embedding, so a background step copies that many between jobs. A
 * derived snapshot may own max(DERIVED_MIN, base / DERIVED_DIVISOR) rows before
 * the next stage is a full one, mirroring the catalog's delta bound. */
enum {
    BACKGROUND_BATCH = 8,
    BACKGROUND_REUSE = 4096,
    SEMANTIC_RESULTS = 10,
    DERIVED_MIN = 4096,
    DERIVED_DIVISOR = 32
};
/* Where a staged row's vector comes from: embedding (cache or model), none
 * (unembeddable before and its text is unchanged), else a row position in the
 * previous snapshot's vector index. */
#define SOURCE_EMBED SIZE_MAX
#define SOURCE_NONE (SIZE_MAX - 1)
/* Set on a source position that indexes the origin's base vectors. */
#define SOURCE_BASE_BIT ((SIZE_MAX >> 1) + 1)
static const double SEMANTIC_MIN_COSINE = 0.2;
struct model {
    tl_embedder *embedder;
    size_t references;
};
struct metadata {
    uint64_t id, revision;
    char *path, *name, *icon, *desktop_id, *text;
    uint64_t text_hash; /* of the prepared embedding text, kept after embedding */
    size_t source;      /* while staging: see SOURCE_EMBED */
    bool is_dir, settings;
};
struct snapshot {
    struct snapshot *next; /* in the service's list of unheld bases */
    /* While staging, the published snapshot whose rows are reused; holds one
     * reference until the stage finishes or is discarded. */
    struct snapshot *origin;
    struct model *model;
    tl_vec *entries;
    tl_vector *vectors;
    tl_vector_workspace *workspace;
    /* A derived snapshot holds one reference on its full base and hides the
     * base entries and vector rows it replaced or removed; entries/vectors
     * above are only its own rows. NULL base: a full snapshot. */
    struct snapshot *base;
    uint64_t *hidden_entries, *hidden_rows;
    size_t hidden_count;
    tl_vector_workspace *base_workspace;
    /* The catalog base its file rows came through and that catalog view's
     * possibly changed ids (ascending), bounding the next stage's diff. */
    uint64_t catalog_base_gen;
    tl_vec *catalog_changes;
    uint64_t catalog_gen, desktop_gen;
    size_t references, bytes, position;
};
enum job_state { JOB_FREE, JOB_PENDING, JOB_RUNNING, JOB_DONE, JOB_ABANDONED };
struct job {
    enum job_state state;
    struct snapshot *snapshot;
    uint64_t token, deadline;
    tl_ipc_request request;
    char search_id[IPC_HISTORY_ID_BYTES + 1];
    tl_rank_candidate lexical[LEXICAL_MAX_RESULTS];
    size_t lexical_count, output_length;
    char *output;
};
struct tl_semantic {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thread;
    bool started, stop;
    int notification;
    tl_semantic_options options;
    char *model_path, *database;
    tl_store *store;
    struct model *model;
    struct stat loaded_stat;
    bool loaded;
    struct snapshot *active, *retired, *stage;
    /* Former active full snapshots now only kept alive as bases of derived
     * snapshots (or by jobs); freed by the worker once unreferenced. */
    struct snapshot *orphans;
    tl_rank *rank;
    struct job jobs[SEMANTIC_CLIENTS];
    tl_semantic_stats progress;
};
static uint64_t now_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
        return 0;
    return (uint64_t)value.tv_sec * 1000 + (uint64_t)value.tv_nsec / 1000000;
}
static void notify(tl_semantic *service) {
    uint64_t value = 1;
    ssize_t written = write(service->notification, &value, sizeof(value));
    (void)written;
}
static void model_release(struct model *model) {
    if (model != NULL && --model->references == 0) {
        embedder_destroy(model->embedder);
        free(model);
    }
}
static void metadata_free(struct metadata *entry) {
    free(entry->path);
    free(entry->name);
    free(entry->icon);
    free(entry->desktop_id);
    free(entry->text);
}
/* Free a snapshot's own resources. References it holds (origin, base) are
 * released separately under the service lock (see release_holds). */
static void snapshot_free(struct snapshot *snapshot) {
    if (snapshot == NULL)
        return;
    struct metadata *entries = vec_data(snapshot->entries);
    for (size_t i = 0; i < vec_count(snapshot->entries); i++)
        metadata_free(&entries[i]);
    vector_workspace_destroy(snapshot->workspace);
    vector_workspace_destroy(snapshot->base_workspace);
    vector_destroy(snapshot->vectors);
    vec_destroy(snapshot->entries);
    vec_destroy(snapshot->catalog_changes);
    free(snapshot->hidden_entries);
    free(snapshot->hidden_rows);
    model_release(snapshot->model);
    free(snapshot);
}
/* Under the service lock: drop the references snapshot holds on others. */
static void release_holds(struct snapshot *snapshot) {
    if (snapshot->origin != NULL)
        snapshot->origin->references--;
    if (snapshot->base != NULL)
        snapshot->base->references--;
    snapshot->origin = snapshot->base = NULL;
}
/* Drop the stage and release its references on reused snapshots. */
static void discard_stage(tl_semantic *service) {
    struct snapshot *stage = service->stage;
    if (stage == NULL)
        return;
    pthread_mutex_lock(&service->lock);
    release_holds(stage);
    pthread_mutex_unlock(&service->lock);
    snapshot_free(stage);
    service->stage = NULL;
}
static bool hidden(const uint64_t *bitmap, size_t position) {
    return bitmap != NULL && ((bitmap[position / 64] >> (position % 64)) & 1U);
}
static void hide(uint64_t *bitmap, size_t position) {
    bitmap[position / 64] |= UINT64_C(1) << (position % 64);
}
/* An entry of one segment by id, with its position there. */
static const struct metadata *find_entry(const struct snapshot *segment, uint64_t id,
                                         size_t *position) {
    const struct metadata *entries = vec_const_data(segment->entries);
    size_t low = 0, high = vec_count(segment->entries);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (entries[middle].id < id)
            low = middle + 1;
        else
            high = middle;
    }
    *position = low;
    return low < vec_count(segment->entries) && entries[low].id == id ? &entries[low] : NULL;
}
/* The live entry for id: own rows first, then unhidden base rows. */
static const struct metadata *resolve(const struct snapshot *snapshot, uint64_t id) {
    size_t position = 0;
    const struct metadata *own = find_entry(snapshot, id, &position);
    if (own != NULL || snapshot->base == NULL)
        return own;
    const struct metadata *base = find_entry(snapshot->base, id, &position);
    return base != NULL && !hidden(snapshot->hidden_entries, position) ? base : NULL;
}
/* Where id's vector lives in snapshot: an own row, a base row (with
 * SOURCE_BASE_BIT), or SOURCE_NONE when it has none. */
static size_t vector_source(const struct snapshot *snapshot, uint64_t id) {
    size_t position = 0;
    if (vector_position(snapshot->vectors, id, &position) == TL_OK)
        return position;
    if (snapshot->base != NULL &&
        vector_position(snapshot->base->vectors, id, &position) == TL_OK &&
        !hidden(snapshot->hidden_rows, position))
        return position | SOURCE_BASE_BIT;
    return SOURCE_NONE;
}
static size_t live_entries(const struct snapshot *snapshot) {
    size_t base = snapshot->base == NULL ? 0 : vec_count(snapshot->base->entries);
    return vec_count(snapshot->entries) + base - snapshot->hidden_count;
}
static tl_status append_metadata(tl_semantic *service, struct snapshot *snapshot,
                                 struct metadata *entry) {
    const char *strings[] = {entry->path, entry->name, entry->icon, entry->desktop_id, entry->text};
    size_t bytes = sizeof(*entry);
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++)
        if (strings[i] != NULL)
            bytes += strlen(strings[i]) + 1;
    tl_status status =
        bytes > service->options.metadata_budget - snapshot->bytes ? TL_LIMIT : TL_OK;
    if (status == TL_OK)
        status = vec_append(snapshot->entries, entry);
    if (status == TL_OK)
        snapshot->bytes += bytes;
    else
        metadata_free(entry);
    return status;
}
/* Fill entry (owned copies) for a catalog file and its prepared text. */
static tl_status file_metadata(uint64_t id, const char *path, bool is_dir, const char *context,
                               struct metadata *entry) {
    char text[EMBED_TEXT_BYTES + 1];
    *entry = (struct metadata){.id = id, .is_dir = is_dir, .source = SOURCE_EMBED};
    tl_status status = potion_prepare_path(context, text, sizeof(text));
    if (status != TL_OK)
        return status;
    entry->path = strdup(path);
    entry->text = strdup(text);
    entry->text_hash = hashmap_hash(HASHMAP_HASH_SEED, text, strlen(text));
    if (entry->path == NULL || entry->text == NULL) {
        metadata_free(entry);
        return TL_NOMEM;
    }
    return TL_OK;
}
static tl_status copy_files(tl_semantic *service, struct snapshot *snapshot,
                            const tl_catalog_snapshot *source) {
    tl_status status = TL_OK;
    for (size_t i = 0; i < catalog_snapshot_count(source) && status == TL_OK; i++) {
        const char *path = NULL;
        uint64_t id = 0;
        bool is_dir = false;
        struct metadata entry = {0};
        status = catalog_snapshot_entry(source, i, &id, &path, &is_dir);
        if (status == TL_OK)
            status = file_metadata(id, path, is_dir, catalog_snapshot_context(source, i), &entry);
        if (status == TL_OK)
            status = append_metadata(service, snapshot, &entry);
    }
    return status;
}
/* Fill entry (owned copies) for an application and its prepared text. */
static tl_status desktop_metadata(const tl_desktop_entry *source, struct metadata *entry) {
    char text[EMBED_TEXT_BYTES + 1];
    int length = snprintf(text, sizeof(text), "%s %s %s", source->name, source->generic_name,
                          source->keywords);
    if (length < 0 || (size_t)length >= sizeof(text))
        return TL_LIMIT;
    for (size_t j = 0; j < (size_t)length; j++)
        if (text[j] == ';')
            text[j] = ' ';
    *entry = (struct metadata){.id = source->id,
                               .revision = source->revision,
                               .settings = source->settings,
                               .path = strdup(source->filename),
                               .name = strdup(source->name),
                               .icon = strdup(source->icon),
                               .desktop_id = strdup(source->desktop_id),
                               .text = strdup(text),
                               .text_hash = hashmap_hash(HASHMAP_HASH_SEED, text, (size_t)length),
                               .source = SOURCE_EMBED};
    if (entry->path == NULL || entry->name == NULL || entry->icon == NULL ||
        entry->desktop_id == NULL || entry->text == NULL) {
        metadata_free(entry);
        return TL_NOMEM;
    }
    return TL_OK;
}
/* Under the desktop lease held by the caller. */
static tl_status copy_desktop(tl_semantic *service, struct snapshot *snapshot) {
    tl_desktop *desktop = service->options.desktop;
    tl_status status = TL_OK;
    for (size_t i = 0; i < desktop_count(desktop) && status == TL_OK; i++) {
        struct metadata entry = {0};
        status = desktop_metadata(desktop_entry(desktop, i), &entry);
        if (status == TL_OK)
            status = append_metadata(service, snapshot, &entry);
    }
    return status;
}
static bool current(tl_semantic *service, const struct snapshot *snapshot) {
    tl_catalog_stats stats;
    if (catalog_stats(service->options.catalog, &stats) != TL_OK || !stats.available ||
        stats.catalog_gen != snapshot->catalog_gen)
        return false;
    desktop_acquire(service->options.desktop);
    bool matches = desktop_gen(service->options.desktop) == snapshot->desktop_gen;
    desktop_release(service->options.desktop);
    return matches;
}
static tl_status stage_descriptor(tl_semantic *service, const tl_emb_model *model) {
    char text[4096];
    tl_json_buffer buffer;
    json_buffer_init(&buffer, text, sizeof(text));
    const char *fields[] = {model->model_id,           model->model_revision,
                            model->tokenizer_version,  model->preprocessing_version,
                            model->projection_version, "int8-l2-1"};
    json_raw(&buffer, "[");
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (i != 0)
            json_raw(&buffer, ",");
        json_quote(&buffer, fields[i]);
    }
    json_raw(&buffer, ",");
    json_number(&buffer, model->dimensions);
    json_raw(&buffer, "]");
    return buffer.status == TL_OK ? store_embedding_stage(service->store, model->emb_gen, text)
                                  : buffer.status;
}
/* ---- stages ---------------------------------------------------------------- */
static tl_status add_change(void *context, uint64_t id) {
    return vec_append(context, &id);
}
/* Sort ids ascending and drop repeats in place. */
static tl_status sort_unique(tl_vec *ids) {
    uint64_t *values = vec_data(ids);
    size_t count = vec_count(ids), unique = 0;
    tl_status status = sort_u64(values, count);
    for (size_t i = 0; i < count && status == TL_OK; i++)
        if (unique == 0 || values[unique - 1] != values[i])
            values[unique++] = values[i];
    vec_truncate(ids, unique);
    return status;
}
/* Record the catalog view's base and the ids that may differ from it. */
static tl_status record_changes(struct snapshot *snapshot, const tl_catalog_snapshot *source) {
    snapshot->catalog_gen = catalog_snapshot_gen(source);
    snapshot->catalog_base_gen = catalog_snapshot_base_gen(source);
    tl_status status = vec_create(sizeof(uint64_t), &snapshot->catalog_changes);
    if (status == TL_OK)
        status = catalog_snapshot_changes(source, add_change, snapshot->catalog_changes);
    return status == TL_OK ? sort_unique(snapshot->catalog_changes) : status;
}
/* Under the service lock: take a reference on target for snapshot. */
static struct snapshot *hold(struct snapshot *target) {
    if (target != NULL)
        target->references++;
    return target;
}
/* Full stage: mark rows whose prepared text the published snapshot already
 * embedded with the same model, so staging copies their vectors (or their lack
 * of one) instead of looking up the cache or running inference. */
static size_t adopt_origin(tl_semantic *service, struct snapshot *snapshot) {
    pthread_mutex_lock(&service->lock);
    if (service->active != NULL && service->active->model == snapshot->model)
        snapshot->origin = hold(service->active);
    pthread_mutex_unlock(&service->lock);
    const struct snapshot *origin = snapshot->origin;
    if (origin == NULL)
        return 0;
    size_t reused = 0;
    struct metadata *entries = vec_data(snapshot->entries);
    for (size_t i = 0; i < vec_count(snapshot->entries); i++) {
        const struct metadata *old = resolve(origin, entries[i].id);
        if (old == NULL || old->text_hash != entries[i].text_hash)
            continue;
        entries[i].source = vector_source(origin, entries[i].id);
        free(entries[i].text);
        entries[i].text = NULL;
        reused++;
    }
    return reused;
}
/* Every row is copied; the desktop lease covers only application rows. */
static tl_status stage_full(tl_semantic *service, struct snapshot *snapshot,
                            const tl_catalog_snapshot *source, size_t *reused) {
    tl_status status = copy_files(service, snapshot, source);
    desktop_acquire(service->options.desktop);
    snapshot->desktop_gen = desktop_gen(service->options.desktop);
    if (status == TL_OK)
        status = copy_desktop(service, snapshot);
    desktop_release(service->options.desktop);
    *reused = status == TL_OK ? adopt_origin(service, snapshot) : 0;
    return status;
}
/* A derived stage's view of what changed since the published origin. */
struct diff {
    tl_semantic *service;
    struct snapshot *stage, *origin, *base;
    const tl_catalog_snapshot *source;
    size_t embed; /* rows that need embedding */
};
static char *copy_text(const char *text, bool *failed) {
    char *copy = text == NULL ? NULL : strdup(text);
    *failed = *failed || (text != NULL && copy == NULL);
    return copy;
}
/* Copy an origin-owned row unchanged into the stage, reusing its vector. */
static tl_status carry(struct diff *diff, const struct metadata *row) {
    bool failed = false;
    struct metadata copy = {.id = row->id,
                            .revision = row->revision,
                            .path = copy_text(row->path, &failed),
                            .name = copy_text(row->name, &failed),
                            .icon = copy_text(row->icon, &failed),
                            .desktop_id = copy_text(row->desktop_id, &failed),
                            .text_hash = row->text_hash,
                            .source = vector_source(diff->origin, row->id),
                            .is_dir = row->is_dir,
                            .settings = row->settings};
    if (failed) {
        metadata_free(&copy);
        return TL_NOMEM;
    }
    return append_metadata(diff->service, diff->stage, &copy);
}
/* Hide id's base entry and vector row in the stage, if the base has them. */
static void hide_base(struct diff *diff, uint64_t id) {
    size_t position = 0;
    struct snapshot *stage = diff->stage;
    if (find_entry(diff->base, id, &position) != NULL && !hidden(stage->hidden_entries, position)) {
        hide(stage->hidden_entries, position);
        stage->hidden_count++;
    }
    if (vector_position(diff->base->vectors, id, &position) == TL_OK)
        hide(stage->hidden_rows, position);
}
/* Current metadata for id from the catalog view or the leased desktop
 * catalog; TL_STATE when the entry is gone. */
static tl_status fresh_entry(struct diff *diff, uint64_t id, struct metadata *out) {
    if (id >= DESKTOP_ID_BASE) {
        const tl_desktop_entry *entry = desktop_resolve(diff->service->options.desktop, id);
        return entry == NULL ? TL_STATE : desktop_metadata(entry, out);
    }
    const char *path = NULL, *context = NULL;
    bool is_dir = false;
    tl_status status = catalog_snapshot_find(diff->source, id, &path, &is_dir, &context);
    return status == TL_OK ? file_metadata(id, path, is_dir, context, out) : status;
}
static bool same_row(const struct metadata *a, const struct metadata *b) {
    return a->text_hash == b->text_hash && a->is_dir == b->is_dir && a->revision == b->revision &&
           strcmp(a->path, b->path) == 0;
}
/* Re-evaluate one id that may have changed: gone ids hide their base row, an
 * unchanged shared base row stays visible, and every other live row becomes
 * an own row hiding any base row. An own row copies the origin's vector when
 * its prepared text is unchanged (as a full stage would) and else is embedded. */
static tl_status update_id(struct diff *diff, uint64_t id) {
    struct metadata fresh;
    tl_status status = fresh_entry(diff, id, &fresh);
    if (status == TL_STATE) {
        hide_base(diff, id);
        return TL_OK;
    }
    if (status != TL_OK)
        return status;
    const struct metadata *view = resolve(diff->origin, id);
    size_t position = 0;
    bool owned = diff->origin->base != NULL && find_entry(diff->origin, id, &position) != NULL;
    if (view != NULL && !owned && same_row(view, &fresh)) {
        metadata_free(&fresh);
        return TL_OK;
    }
    hide_base(diff, id);
    if (view != NULL && view->text_hash == fresh.text_hash) {
        fresh.source = vector_source(diff->origin, id);
        free(fresh.text);
        fresh.text = NULL;
    } else {
        diff->embed++;
    }
    return append_metadata(diff->service, diff->stage, &fresh);
}
/* Merge the origin's own rows in [low, high) ids with the candidate ids:
 * untouched own rows are carried, candidates re-evaluated, in id order. */
static tl_status merge_rows(struct diff *diff, const uint64_t *ids, size_t count, uint64_t low,
                            uint64_t high) {
    const struct metadata *owned = NULL;
    size_t owned_count = 0, i = 0, j = 0;
    if (diff->origin->base != NULL) {
        owned = vec_const_data(diff->origin->entries);
        owned_count = vec_count(diff->origin->entries);
    }
    while (i < owned_count && owned[i].id < low)
        i++;
    tl_status status = TL_OK;
    while (status == TL_OK && ((i < owned_count && owned[i].id < high) || j < count)) {
        bool own_next = i < owned_count && owned[i].id < high;
        if (own_next && (j == count || owned[i].id < ids[j])) {
            status = carry(diff, &owned[i++]);
            continue;
        }
        if (own_next && owned[i].id == ids[j])
            i++;
        status = update_id(diff, ids[j++]);
    }
    return status;
}
/* All application ids of the leased desktop catalog and the origin's view. */
static tl_status desktop_candidates(struct diff *diff, tl_vec *ids) {
    tl_desktop *desktop = diff->service->options.desktop;
    tl_status status = TL_OK;
    for (size_t i = 0; i < desktop_count(desktop) && status == TL_OK; i++) {
        uint64_t id = desktop_entry(desktop, i)->id;
        status = vec_append(ids, &id);
    }
    const struct snapshot *segments[] = {diff->origin, diff->base};
    for (size_t s = 0; s < 2 && status == TL_OK; s++) {
        size_t position = 0;
        find_entry(segments[s], DESKTOP_ID_BASE, &position);
        const struct metadata *entries = vec_const_data(segments[s]->entries);
        for (; position < vec_count(segments[s]->entries) && status == TL_OK; position++)
            status = vec_append(ids, &entries[position].id);
    }
    return status == TL_OK ? sort_unique(ids) : status;
}
/* Files first (no desktop lease), then applications under a short lease;
 * applications are re-evaluated only when the desktop catalog changed. */
static tl_status diff_rows(struct diff *diff, tl_vec *candidates) {
    tl_status status =
        merge_rows(diff, vec_const_data(candidates), vec_count(candidates), 0, DESKTOP_ID_BASE);
    tl_vec *apps = NULL;
    if (status == TL_OK)
        status = vec_create(sizeof(uint64_t), &apps);
    tl_desktop *desktop = diff->service->options.desktop;
    desktop_acquire(desktop);
    diff->stage->desktop_gen = desktop_gen(desktop);
    if (status == TL_OK && diff->stage->desktop_gen != diff->origin->desktop_gen)
        status = desktop_candidates(diff, apps);
    if (status == TL_OK)
        status =
            merge_rows(diff, vec_const_data(apps), vec_count(apps), DESKTOP_ID_BASE, UINT64_MAX);
    desktop_release(desktop);
    vec_destroy(apps);
    return status;
}
static tl_status copy_hidden(struct snapshot *stage, const struct snapshot *origin,
                             const struct snapshot *base) {
    size_t entry_words = vec_count(base->entries) / 64 + 1;
    size_t row_words = vector_count(base->vectors) / 64 + 1;
    stage->hidden_entries = calloc(entry_words, sizeof(uint64_t));
    stage->hidden_rows = calloc(row_words, sizeof(uint64_t));
    if (stage->hidden_entries == NULL || stage->hidden_rows == NULL)
        return TL_NOMEM;
    if (origin->base != NULL) {
        memcpy(stage->hidden_entries, origin->hidden_entries, entry_words * sizeof(uint64_t));
        memcpy(stage->hidden_rows, origin->hidden_rows, row_words * sizeof(uint64_t));
        stage->hidden_count = origin->hidden_count;
    }
    return TL_OK;
}
/* Ids that may differ between the origin's catalog view and the new one. */
static tl_status file_candidates(const struct snapshot *origin, const struct snapshot *stage,
                                 tl_vec **out) {
    tl_status status = vec_create(sizeof(uint64_t), out);
    if (status == TL_OK)
        status = vec_append_array(*out, vec_const_data(origin->catalog_changes),
                                  vec_count(origin->catalog_changes));
    if (status == TL_OK)
        status = vec_append_array(*out, vec_const_data(stage->catalog_changes),
                                  vec_count(stage->catalog_changes));
    return status == TL_OK ? sort_unique(*out) : status;
}
/* Stage a derived snapshot over the published one when both catalog views
 * share a base. TL_LIMIT when a full stage is needed instead. */
static tl_status stage_derived(tl_semantic *service, struct snapshot *stage,
                               const tl_catalog_snapshot *source, size_t *reused) {
    pthread_mutex_lock(&service->lock);
    struct snapshot *origin = service->active;
    bool usable = origin != NULL && origin->model == stage->model &&
                  origin->catalog_base_gen == stage->catalog_base_gen;
    if (usable) {
        stage->origin = hold(origin);
        stage->base = hold(origin->base != NULL ? origin->base : origin);
    }
    pthread_mutex_unlock(&service->lock);
    if (!usable)
        return TL_LIMIT;
    struct diff diff = {service, stage, origin, stage->base, source, 0};
    tl_vec *candidates = NULL;
    tl_status status = copy_hidden(stage, origin, stage->base);
    if (status == TL_OK)
        status = file_candidates(origin, stage, &candidates);
    if (status == TL_OK)
        status = diff_rows(&diff, candidates);
    vec_destroy(candidates);
    size_t bound = vec_count(stage->base->entries) / DERIVED_DIVISOR;
    if (status == TL_OK && vec_count(stage->entries) > (bound < DERIVED_MIN ? DERIVED_MIN : bound))
        status = TL_LIMIT;
    *reused = live_entries(stage) - diff.embed;
    return status;
}
static struct snapshot *new_stage(tl_semantic *service) {
    struct snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL)
        return NULL;
    if (vec_create(sizeof(struct metadata), &snapshot->entries) != TL_OK) {
        free(snapshot);
        return NULL;
    }
    snapshot->model = service->model;
    snapshot->model->references++;
    return snapshot;
}
/* Prepare the stage's rows from the pinned catalog view: derived when the
 * published snapshot shares its catalog base and the delta stays small,
 * otherwise full. *out is NULL on failure. */
static tl_status prepare_rows(tl_semantic *service, const tl_catalog_snapshot *source,
                              struct snapshot **out, size_t *reused) {
    *out = new_stage(service);
    tl_status status = *out == NULL ? TL_NOMEM : record_changes(*out, source);
    if (status == TL_OK)
        status = stage_derived(service, *out, source, reused);
    if (status == TL_LIMIT) {
        service->stage = *out;
        discard_stage(service);
        *out = new_stage(service);
        status = *out == NULL ? TL_NOMEM : record_changes(*out, source);
        if (status == TL_OK)
            status = stage_full(service, *out, source, reused);
    }
    return status;
}
static void stage_failed(tl_semantic *service, tl_status status) {
    discard_stage(service);
    pthread_mutex_lock(&service->lock);
    service->progress.building = false;
    service->progress.last_error = status;
    pthread_mutex_unlock(&service->lock);
}
static tl_status begin_stage(tl_semantic *service) {
    pthread_mutex_lock(&service->lock);
    service->progress.building = true;
    service->progress.processed = 0;
    service->progress.total = 0;
    pthread_mutex_unlock(&service->lock);
    tl_catalog_snapshot *source = NULL;
    struct snapshot *snapshot = NULL;
    size_t reused = 0;
    tl_status status = catalog_pin(service->options.catalog, &source);
    if (status == TL_OK)
        status = prepare_rows(service, source, &snapshot, &reused);
    catalog_unpin(source);
    service->stage = snapshot;
    const tl_emb_model *model = embedder_model(service->model->embedder);
    if (status == TL_OK)
        status = vector_create_int8(model->emb_gen, model->dimensions, vec_count(snapshot->entries),
                                    service->options.vector_budget, &snapshot->vectors);
    /* Only a stage that reuses nothing re-stages the descriptor: reused rows
     * never touch the cache, so only such a stage may sweep it. */
    if (status == TL_OK && snapshot->origin == NULL)
        status = stage_descriptor(service, model);
    if (status != TL_OK) {
        stage_failed(service, status);
        return status;
    }
    pthread_mutex_lock(&service->lock);
    service->progress.reused = reused;
    pthread_mutex_unlock(&service->lock);
    return TL_OK;
}
/* Copy the next row's vector from the origin when the stage reuses it.
 * Returns false (doing nothing) for a row that needs embedding. */
static bool reuse_row(struct snapshot *snapshot, tl_status *status) {
    struct metadata *entry = (struct metadata *)vec_data(snapshot->entries) + snapshot->position;
    if (entry->source == SOURCE_EMBED)
        return false;
    const struct snapshot *origin = snapshot->origin;
    const tl_vector *from = (entry->source & SOURCE_BASE_BIT) != 0 && entry->source != SOURCE_NONE
                                ? origin->base->vectors
                                : origin->vectors;
    *status = entry->source == SOURCE_NONE
                  ? TL_OK
                  : vector_add_row(snapshot->vectors, from, entry->source & ~SOURCE_BASE_BIT);
    if (*status == TL_OK)
        snapshot->position++;
    return true;
}
static tl_status embed_row(tl_semantic *service, struct snapshot *snapshot) {
    tl_status reused = TL_OK;
    if (reuse_row(snapshot, &reused))
        return reused;
    struct metadata *entries = vec_data(snapshot->entries);
    struct metadata *entry = &entries[snapshot->position];
    const tl_emb_model *model = embedder_model(snapshot->model->embedder);
    float values[VECTOR_MAX_DIMENSIONS];
    bool found = false;
    tl_status status = store_embedding_get(service->store, model->emb_gen, entry->text, values,
                                           model->dimensions, &found);
    if (status == TL_OK && !found) {
        status = embedder_encode(snapshot->model->embedder, EMBED_DOCUMENT, entry->text, values,
                                 model->dimensions);
        if (status == TL_STATE || status == TL_INVALID)
            status = TL_OK; /* Unembeddable paths keep their lexical metadata. */
        else if (status == TL_OK) {
            found = true;
            status = store_embedding_put(service->store, model->emb_gen, entry->text, values,
                                         model->dimensions);
        }
    }
    if (status == TL_OK && found)
        status =
            vector_add(snapshot->vectors, entry->id, model->emb_gen, values, model->dimensions);
    if (status == TL_OK) {
        free(entry->text);
        entry->text = NULL;
        snapshot->position++;
    }
    return status;
}
/* Free the retired snapshot and unheld bases nobody references any more. A
 * freed derived snapshot releases its base, which a later pass frees. */
static void reclaim(tl_semantic *service) {
    pthread_mutex_lock(&service->lock);
    struct snapshot *garbage = service->retired;
    if (garbage != NULL && garbage->references == 0) {
        service->retired = NULL;
        release_holds(garbage);
        garbage->next = NULL;
    } else {
        garbage = NULL;
    }
    for (struct snapshot **link = &service->orphans; *link != NULL;) {
        struct snapshot *orphan = *link;
        if (orphan->references != 0) {
            link = &orphan->next;
            continue;
        }
        *link = orphan->next;
        orphan->next = garbage;
        garbage = orphan;
    }
    pthread_mutex_unlock(&service->lock);
    while (garbage != NULL) {
        struct snapshot *next = garbage->next;
        snapshot_free(garbage);
        garbage = next;
    }
}
static tl_status finish_stage(tl_semantic *service) {
    struct snapshot *snapshot = service->stage;
    tl_status status = vector_finish(snapshot->vectors);
    if (status == TL_OK)
        status = vector_workspace_create(snapshot->vectors, &snapshot->workspace);
    /* A derived snapshot searches its base through its own scratch, hiding
     * the base rows it replaced. */
    if (status == TL_OK && snapshot->base != NULL)
        status = vector_workspace_create(snapshot->base->vectors, &snapshot->base_workspace);
    vector_workspace_exclude(snapshot->base_workspace, snapshot->hidden_rows);
    if (status == TL_OK && snapshot->origin == NULL)
        status = store_embedding_activate(service->store,
                                          embedder_model(snapshot->model->embedder)->emb_gen);
    if (status != TL_OK)
        return status;
    pthread_mutex_lock(&service->lock);
    if (snapshot->origin != NULL)
        snapshot->origin->references--;
    snapshot->origin = NULL;
    struct snapshot *previous = service->active;
    /* A full snapshot that becomes the base of its successor leaves service
     * ownership: the successor's reference keeps it, and reclaim frees it once
     * unreferenced. Any other predecessor retires as before. */
    if (previous != NULL && previous == snapshot->base) {
        previous->next = service->orphans;
        service->orphans = previous;
    } else {
        service->retired = previous;
    }
    service->active = snapshot;
    service->stage = NULL;
    if (snapshot->base != NULL)
        service->progress.derived_stages++;
    else
        service->progress.full_stages++;
    pthread_mutex_unlock(&service->lock);
    notify(service);
    return TL_OK;
}
static void encode_metadata(tl_json_buffer *buffer, const struct metadata *entry) {
    ipc_result(buffer, entry->id, entry->path);
    if (buffer->status != TL_OK)
        return;
    buffer->data[--buffer->length] = 0;
    json_raw(buffer, ",\"kind\":");
    json_quote(buffer, entry->name == NULL ? (entry->is_dir ? "folder" : "file")
                                           : (entry->settings ? "settings" : "application"));
    if (entry->name != NULL) {
        json_raw(buffer, ",\"name\":");
        json_quote(buffer, entry->name);
        json_raw(buffer, ",\"icon\":");
        json_quote(buffer, entry->icon);
        json_raw(buffer, ",\"desktop_id\":");
        json_quote(buffer, entry->desktop_id);
        json_raw(buffer, ",\"desktop_revision\":\"");
        json_number(buffer, entry->revision);
        json_raw(buffer, "\"");
    }
    json_raw(buffer, "}");
}
static tl_status encode_response(const struct job *job, const tl_rank_result *results, size_t count,
                                 const char *status, const char *reason, char *out, size_t capacity,
                                 size_t *length) {
    tl_json_buffer buffer;
    json_buffer_init(&buffer, out,
                     capacity < SEMANTIC_RESPONSE_BYTES ? capacity : SEMANTIC_RESPONSE_BYTES);
    json_raw(&buffer, "{\"version\":1,\"request_id\":");
    json_quote(&buffer, job->request.request_id);
    json_raw(&buffer, ",\"phase\":\"final\",\"catalog_gen\":");
    json_number(&buffer, job->snapshot->catalog_gen);
    json_raw(&buffer, ",\"emb_gen\":");
    json_number(&buffer, embedder_model(job->snapshot->model->embedder)->emb_gen);
    json_raw(&buffer, ",\"search_id\":");
    json_quote(&buffer, job->search_id);
    json_raw(&buffer, ",\"status\":");
    json_quote(&buffer, status);
    json_raw(&buffer, ",\"reason\":");
    json_quote(&buffer, reason);
    json_raw(&buffer, ",\"results\":[");
    for (size_t i = 0; i < count && i < job->request.limit; i++) {
        if (i != 0)
            json_raw(&buffer, ",");
        uint64_t id = results == NULL ? job->lexical[i].id : results[i].id;
        const struct metadata *entry = resolve(job->snapshot, id);
        if (entry == NULL)
            return TL_STATE;
        encode_metadata(&buffer, entry);
    }
    json_raw(&buffer, "]}\n");
    *length = buffer.status == TL_OK ? buffer.length : 0;
    return buffer.status;
}
/* vector_query's order: higher cosine first, then lower id. */
static bool ranks_before(const tl_vector_result *a, const tl_vector_result *b) {
    return a->cosine > b->cosine || (a->cosine == b->cosine && a->id < b->id);
}
/* Best hits over both segments of a snapshot, in vector_query's order. Each
 * segment returns its own best, so the merge is exact. */
static tl_status search(const struct snapshot *snapshot, const tl_emb_model *model,
                        const float *query, tl_vector_result *hits, size_t *count) {
    tl_vector_result own[SEMANTIC_RESULTS], base[SEMANTIC_RESULTS];
    size_t own_count = 0, base_count = 0;
    tl_status status = vector_query(snapshot->vectors, snapshot->workspace, model->emb_gen, query,
                                    model->dimensions, own, SEMANTIC_RESULTS, &own_count);
    if (status == TL_OK && snapshot->base != NULL)
        status = vector_query(snapshot->base->vectors, snapshot->base_workspace, model->emb_gen,
                              query, model->dimensions, base, SEMANTIC_RESULTS, &base_count);
    size_t i = 0, j = 0;
    *count = 0;
    while (status == TL_OK && *count < SEMANTIC_RESULTS && (i < own_count || j < base_count)) {
        bool take_own = j == base_count || (i < own_count && ranks_before(&own[i], &base[j]));
        hits[(*count)++] = take_own ? own[i++] : base[j++];
    }
    return status;
}
static void run_job(tl_semantic *service, struct job *job) {
    const tl_emb_model *model = embedder_model(job->snapshot->model->embedder);
    float query[VECTOR_MAX_DIMENSIONS];
    tl_status status = embedder_encode(job->snapshot->model->embedder, EMBED_QUERY,
                                       job->request.query, query, model->dimensions);
    tl_vector_result hits[SEMANTIC_RESULTS];
    size_t count = 0;
    if (status == TL_OK)
        status = search(job->snapshot, model, query, hits, &count);
    tl_rank_candidate semantic[SEMANTIC_RESULTS];
    size_t accepted = 0;
    for (size_t i = 0; i < count && status == TL_OK; i++) {
        if (hits[i].cosine < SEMANTIC_MIN_COSINE)
            break; /* Threshold selected on tuning families only. */
        const struct metadata *entry = resolve(job->snapshot, hits[i].id);
        if (entry == NULL) {
            status = TL_STATE;
            break;
        }
        semantic[accepted++] = (tl_rank_candidate){entry->id, entry->path, RANK_REGULAR};
    }
    tl_rank_result fused[LEXICAL_MAX_RESULTS];
    size_t fused_count = 0;
    if (status == TL_OK)
        status = rank_fuse(service->rank, job->lexical, job->lexical_count, semantic, accepted,
                           fused, job->request.limit, &fused_count);
    const char *reason = status != TL_OK ? "semantic_error"
                         : accepted == 0 ? "semantic_no_match"
                                         : "hybrid";
    tl_status encoded = encode_response(
        job, status == TL_OK ? fused : NULL, status == TL_OK ? fused_count : job->lexical_count,
        "ok", reason, job->output, SEMANTIC_RESPONSE_BYTES, &job->output_length);
    if (encoded != TL_OK) {
        encoded = encode_response(job, NULL, 0, "error", "response_limit", job->output,
                                  SEMANTIC_RESPONSE_BYTES, &job->output_length);
        (void)encoded;
    }
    pthread_mutex_lock(&service->lock);
    if (job->state == JOB_ABANDONED) {
        job->snapshot->references--;
        job->state = JOB_FREE;
    } else {
        job->state = JOB_DONE;
    }
    pthread_mutex_unlock(&service->lock);
    notify(service);
}
static struct job *next_job(tl_semantic *service) {
    pthread_mutex_lock(&service->lock);
    struct job *next = NULL;
    for (size_t i = 0; i < SEMANTIC_CLIENTS; i++) {
        if (service->jobs[i].state != JOB_PENDING)
            continue;
        if (next == NULL || service->jobs[i].deadline < next->deadline)
            next = &service->jobs[i];
    }
    if (next != NULL)
        next->state = JOB_RUNNING;
    pthread_mutex_unlock(&service->lock);
    return next;
}
static bool stopped(tl_semantic *service) {
    pthread_mutex_lock(&service->lock);
    bool stop = service->stop;
    pthread_mutex_unlock(&service->lock);
    return stop;
}
static tl_status refresh_model(tl_semantic *service) {
    struct stat info;
    if (stat(service->model_path, &info) != 0)
        return TL_IO;
    if (service->loaded && info.st_ino == service->loaded_stat.st_ino &&
        info.st_size == service->loaded_stat.st_size &&
        info.st_mtim.tv_sec == service->loaded_stat.st_mtim.tv_sec &&
        info.st_mtim.tv_nsec == service->loaded_stat.st_mtim.tv_nsec)
        return TL_OK;
    struct model *model = calloc(1, sizeof(*model));
    if (model == NULL)
        return TL_NOMEM;
    tl_status status = potion_load(service->model_path, POTION_MODEL_BYTES, &model->embedder);
    if (status != TL_OK) {
        free(model);
        return status;
    }
    model->references = 1;
    model_release(service->model);
    service->model = model;
    service->loaded_stat = info;
    service->loaded = true;
    discard_stage(service);
    return TL_OK;
}
static void update_progress(tl_semantic *service, tl_status status) {
    pthread_mutex_lock(&service->lock);
    service->progress.building = service->stage != NULL;
    service->progress.last_error = status;
    if (service->stage != NULL) {
        service->progress.processed = service->stage->position;
        service->progress.total = vec_count(service->stage->entries);
    } else if (status == TL_OK && service->active != NULL) {
        service->progress.total = live_entries(service->active);
        service->progress.processed = service->progress.total;
    }
    pthread_mutex_unlock(&service->lock);
}
static tl_status prepare_background(tl_semantic *service) {
    reclaim(service);
    if (service->store == NULL) {
        tl_status status = store_create(service->database, &service->store);
        if (status != TL_OK)
            return status;
    }
    tl_status status = refresh_model(service);
    if (status != TL_OK)
        return status;
    if (service->stage != NULL && !current(service, service->stage))
        discard_stage(service);
    if (service->retired != NULL)
        return TL_OK; /* Publication waits instead of accumulating old views. */
    if (service->stage != NULL)
        return TL_OK;
    if (service->active != NULL && service->active->model == service->model &&
        current(service, service->active))
        return TL_OK;
    return begin_stage(service);
}
/* Copy up to BACKGROUND_REUSE consecutive reused rows without SQLite. */
static tl_status reuse_rows(struct snapshot *stage) {
    tl_status status = TL_OK;
    for (size_t i = 0; i < BACKGROUND_REUSE && status == TL_OK &&
                       stage->position < vec_count(stage->entries) && reuse_row(stage, &status);
         i++) {
    }
    return status;
}
static tl_status background(tl_semantic *service) {
    tl_status status = prepare_background(service);
    if (status == TL_OK && service->stage != NULL)
        status = reuse_rows(service->stage);
    if (status == TL_OK && service->stage != NULL &&
        service->stage->position == vec_count(service->stage->entries))
        status = finish_stage(service);
    if (status != TL_OK || service->stage == NULL) {
        if (status != TL_OK)
            discard_stage(service);
        update_progress(service, status);
        return status;
    }
    status = store_embedding_batch_begin(service->store);
    if (status != TL_OK) {
        /* No row changed before BEGIN succeeds. Keep the resident progress when
         * a reconciliation holds SQLite's writer lock beyond its busy timeout. */
        update_progress(service, status);
        return status;
    }
    for (size_t i = 0; i < BACKGROUND_BATCH && status == TL_OK &&
                       service->stage->position < vec_count(service->stage->entries);
         i++)
        status = embed_row(service, service->stage);
    tl_status committed = store_embedding_batch_end(service->store, status == TL_OK);
    if (committed != TL_OK)
        status = committed;
    if (status == TL_OK && service->stage->position == vec_count(service->stage->entries))
        status = finish_stage(service);
    if (status != TL_OK)
        discard_stage(service);
    update_progress(service, status);
    return status;
}

static void *worker(void *context) {
    tl_semantic *service = context;
    while (!stopped(service)) {
        struct job *job = next_job(service);
        if (job != NULL) {
            run_job(service, job);
            continue;
        }
        tl_status status = background(service);
        if (service->stage != NULL && status == TL_OK)
            continue;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 20000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        pthread_mutex_lock(&service->lock);
        if (!service->stop)
            pthread_cond_timedwait(&service->wake, &service->lock, &deadline);
        pthread_mutex_unlock(&service->lock);
    }
    return NULL;
}

tl_status semantic_create(const tl_semantic_options *options, tl_semantic **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (options == NULL || options->model_path == NULL || options->database == NULL ||
        options->catalog == NULL || options->desktop == NULL || options->vector_budget == 0 ||
        options->metadata_budget == 0 || options->deadline_ms == 0)
        return TL_INVALID;
    tl_semantic *service = calloc(1, sizeof(*service));
    if (service == NULL)
        return TL_NOMEM;
    service->notification = -1;
    if (pthread_mutex_init(&service->lock, NULL) != 0) {
        free(service);
        return TL_IO;
    }
    if (pthread_cond_init(&service->wake, NULL) != 0) {
        pthread_mutex_destroy(&service->lock);
        free(service);
        return TL_IO;
    }
    service->options = *options;
    service->model_path = strdup(options->model_path);
    service->database = strdup(options->database);
    service->notification = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    tl_status status = service->model_path == NULL || service->database == NULL ? TL_NOMEM : TL_OK;
    if (status == TL_OK && service->notification < 0)
        status = TL_IO;
    if (status == TL_OK)
        status = rank_create(RANK_MAX_CANDIDATES, RANK_DEFAULT_RRF_K, &service->rank);
    for (size_t i = 0; i < SEMANTIC_CLIENTS && status == TL_OK; i++) {
        service->jobs[i].output = malloc(IPC_RESPONSE_BYTES);
        if (service->jobs[i].output == NULL)
            status = TL_NOMEM;
    }
    if (status == TL_OK) {
        if (pthread_create(&service->thread, NULL, worker, service) != 0)
            status = TL_IO;
        else
            service->started = true;
    }
    if (status != TL_OK) {
        semantic_destroy(service);
        return status;
    }
    *out = service;
    return TL_OK;
}
void semantic_destroy(tl_semantic *service) {
    if (service == NULL)
        return;
    if (service->started) {
        pthread_mutex_lock(&service->lock);
        service->stop = true;
        pthread_cond_signal(&service->wake);
        pthread_mutex_unlock(&service->lock);
        pthread_join(service->thread, NULL);
    }
    for (size_t i = 0; i < SEMANTIC_CLIENTS; i++)
        free(service->jobs[i].output);
    discard_stage(service);
    while (service->orphans != NULL) {
        struct snapshot *next = service->orphans->next;
        snapshot_free(service->orphans);
        service->orphans = next;
    }
    snapshot_free(service->active);
    snapshot_free(service->retired);
    model_release(service->model);
    store_destroy(service->store);
    rank_destroy(service->rank);
    if (service->notification >= 0)
        close(service->notification);
    pthread_cond_destroy(&service->wake);
    pthread_mutex_destroy(&service->lock);
    free(service->model_path);
    free(service->database);
    free(service);
}
tl_status semantic_submit(tl_semantic *service, size_t slot, uint64_t token,
                          const tl_ipc_request *request, const char *search_id,
                          uint64_t catalog_gen, uint64_t desktop_gen, const tl_result *lexical,
                          size_t count, uint64_t *out_emb_gen) {
    if (out_emb_gen != NULL)
        *out_emb_gen = 0;
    if (service == NULL || slot >= SEMANTIC_CLIENTS || token == 0 || request == NULL ||
        request->operation != IPC_QUERY || request->limit == 0 ||
        request->limit > LEXICAL_MAX_RESULTS || search_id == NULL ||
        strlen(search_id) > IPC_HISTORY_ID_BYTES || (lexical == NULL && count != 0) ||
        count > LEXICAL_MAX_RESULTS || out_emb_gen == NULL)
        return TL_INVALID;
    /* Very short/byte-path queries stay on the lexical path. */
    if (strlen(request->query) < 3 || !json_utf8(request->query) ||
        strchr(request->query, '/') != NULL)
        return TL_STATE;
    pthread_mutex_lock(&service->lock);
    struct job *job = &service->jobs[slot];
    struct snapshot *snapshot = service->active;
    tl_status status = job->state != JOB_FREE ? TL_LIMIT : snapshot == NULL ? TL_STATE : TL_OK;
    if (status == TL_OK &&
        (snapshot->catalog_gen != catalog_gen || snapshot->desktop_gen != desktop_gen))
        status = TL_STATE;
    for (size_t i = 0; i < count && status == TL_OK; i++) {
        const struct metadata *entry = resolve(snapshot, lexical[i].id);
        if (entry == NULL)
            status = TL_STATE;
        else {
            tl_lexical_exactness tier = lexical_exactness(&lexical[i]);
            job->lexical[i] = (tl_rank_candidate){entry->id, entry->path,
                                                  tier == LEXICAL_EXACT_RAW_PATH ? RANK_EXACT_PATH
                                                  : tier == LEXICAL_EXACT_NAME ? RANK_EXACT_BASENAME
                                                                               : RANK_REGULAR};
        }
    }
    if (status == TL_OK) {
        job->snapshot = snapshot;
        snapshot->references++;
        job->token = token;
        job->request = *request;
        memcpy(job->search_id, search_id, strlen(search_id) + 1);
        job->lexical_count = count;
        job->deadline = now_ms() + service->options.deadline_ms;
        job->output_length = 0;
        job->state = JOB_PENDING;
        *out_emb_gen = embedder_model(snapshot->model->embedder)->emb_gen;
        pthread_cond_signal(&service->wake);
    }
    pthread_mutex_unlock(&service->lock);
    return status;
}
tl_status semantic_take(tl_semantic *service, size_t slot, uint64_t token, bool cancel,
                        char *output, size_t capacity, size_t *out_length) {
    if (out_length != NULL)
        *out_length = 0;
    if (service == NULL || slot >= SEMANTIC_CLIENTS || output == NULL || capacity == 0 ||
        out_length == NULL)
        return TL_INVALID;
    pthread_mutex_lock(&service->lock);
    struct job *job = &service->jobs[slot];
    tl_status status = TL_OK;
    if (job->state == JOB_FREE || job->state == JOB_ABANDONED || job->token != token) {
        status = TL_STATE;
    } else if (cancel || now_ms() >= job->deadline) {
        status = encode_response(
            job, NULL, cancel ? 0 : job->lexical_count, cancel ? "cancelled" : "ok",
            cancel ? "superseded" : "semantic_deadline", output, capacity, out_length);
        if (status != TL_OK) {
            pthread_mutex_unlock(&service->lock);
            return status;
        }
        if (job->state == JOB_RUNNING)
            job->state = JOB_ABANDONED;
        else {
            job->snapshot->references--;
            job->state = JOB_FREE;
        }
    } else if (job->state == JOB_DONE) {
        if (job->output_length >= capacity) {
            pthread_mutex_unlock(&service->lock);
            return TL_LIMIT;
        }
        memcpy(output, job->output, job->output_length);
        output[job->output_length] = 0;
        *out_length = job->output_length;
        job->snapshot->references--;
        job->state = JOB_FREE;
    }
    pthread_mutex_unlock(&service->lock);
    return status;
}
int semantic_descriptor(const tl_semantic *service) {
    return service == NULL ? -1 : service->notification;
}
void semantic_drain(tl_semantic *service) {
    if (service == NULL)
        return;
    uint64_t count = 0;
    while (read(service->notification, &count, sizeof(count)) == (ssize_t)sizeof(count)) {
    }
}

tl_status semantic_stats(tl_semantic *service, tl_semantic_stats *out) {
    if (service == NULL || out == NULL)
        return TL_INVALID;
    pthread_mutex_lock(&service->lock);
    *out = service->progress;
    if (service->active != NULL) {
        out->available = true;
        out->emb_gen = embedder_model(service->active->model->embedder)->emb_gen;
        out->catalog_gen = service->active->catalog_gen;
        out->desktop_gen = service->active->desktop_gen;
        const struct snapshot *active = service->active;
        out->entries = live_entries(active);
        out->vector_bytes = vector_bytes(active->vectors) +
                            (active->base == NULL ? 0 : vector_bytes(active->base->vectors));
    }
    pthread_mutex_unlock(&service->lock);
    return TL_OK;
}
