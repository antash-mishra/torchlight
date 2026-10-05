/* Serialized inference gives interactive jobs priority between background rows.
 * Published snapshots own copied metadata, so two phases need no long lexical
 * workspace or desktop lock. Retirement is bounded and reclaimed by the worker. */
#include "torchlight/semantic.h"
#include "torchlight/potion.h"
#include "torchlight/rank.h"
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

enum { BACKGROUND_BATCH = 8, SEMANTIC_RESULTS = 10 };
static const double SEMANTIC_MIN_COSINE = 0.2;
struct model {
    tl_embedder *embedder;
    size_t references;
};
struct metadata {
    uint64_t id, revision;
    char *path, *name, *icon, *desktop_id, *text;
    bool is_dir, settings;
};
struct snapshot {
    struct snapshot *next;
    struct model *model;
    tl_vec *entries;
    tl_vector *vectors;
    tl_vector_workspace *workspace;
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
static void snapshot_free(struct snapshot *snapshot) {
    if (snapshot == NULL)
        return;
    struct metadata *entries = vec_data(snapshot->entries);
    for (size_t i = 0; i < vec_count(snapshot->entries); i++)
        metadata_free(&entries[i]);
    vector_workspace_destroy(snapshot->workspace);
    vector_destroy(snapshot->vectors);
    vec_destroy(snapshot->entries);
    model_release(snapshot->model);
    free(snapshot);
}
static const struct metadata *resolve(const struct snapshot *snapshot, uint64_t id) {
    const struct metadata *entries = vec_const_data(snapshot->entries);
    size_t low = 0, high = vec_count(snapshot->entries);
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (entries[middle].id < id)
            low = middle + 1;
        else
            high = middle;
    }
    return low < vec_count(snapshot->entries) && entries[low].id == id ? &entries[low] : NULL;
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
static tl_status copy_files(tl_semantic *service, struct snapshot *snapshot) {
    tl_catalog_snapshot *source = NULL;
    tl_status status = catalog_pin(service->options.catalog, &source);
    if (status != TL_OK)
        return status;
    snapshot->catalog_gen = catalog_snapshot_gen(source);
    for (size_t i = 0; i < catalog_snapshot_count(source) && status == TL_OK; i++) {
        const char *path = NULL;
        struct metadata entry = {0};
        status = catalog_snapshot_entry(source, i, &entry.id, &path, &entry.is_dir);
        char text[EMBED_TEXT_BYTES + 1];
        if (status == TL_OK)
            status = potion_prepare_path(catalog_snapshot_context(source, i), text, sizeof(text));
        if (status != TL_OK)
            break;
        entry.path = strdup(path);
        entry.text = strdup(text);
        if (entry.path == NULL || entry.text == NULL) {
            metadata_free(&entry);
            status = TL_NOMEM;
        } else {
            status = append_metadata(service, snapshot, &entry);
        }
    }
    catalog_unpin(source);
    return status;
}
static tl_status copy_desktop(tl_semantic *service, struct snapshot *snapshot) {
    tl_desktop *desktop = service->options.desktop;
    desktop_acquire(desktop);
    snapshot->desktop_gen = desktop_gen(desktop);
    tl_status status = TL_OK;
    for (size_t i = 0; i < desktop_count(desktop) && status == TL_OK; i++) {
        const tl_desktop_entry *source = desktop_entry(desktop, i);
        char text[EMBED_TEXT_BYTES + 1];
        int length = snprintf(text, sizeof(text), "%s %s %s", source->name, source->generic_name,
                              source->keywords);
        if (length < 0 || (size_t)length >= sizeof(text)) {
            status = TL_LIMIT;
            break;
        }
        for (size_t j = 0; j < (size_t)length; j++)
            if (text[j] == ';')
                text[j] = ' ';
        struct metadata entry = {.id = source->id,
                                 .revision = source->revision,
                                 .settings = source->settings,
                                 .path = strdup(source->filename),
                                 .name = strdup(source->name),
                                 .icon = strdup(source->icon),
                                 .desktop_id = strdup(source->desktop_id),
                                 .text = strdup(text)};
        if (entry.path == NULL || entry.name == NULL || entry.icon == NULL ||
            entry.desktop_id == NULL || entry.text == NULL) {
            metadata_free(&entry);
            status = TL_NOMEM;
        } else {
            status = append_metadata(service, snapshot, &entry);
        }
    }
    desktop_release(desktop);
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
static tl_status begin_stage(tl_semantic *service) {
    pthread_mutex_lock(&service->lock);
    service->progress.building = true;
    service->progress.processed = 0;
    service->progress.total = 0;
    pthread_mutex_unlock(&service->lock);
    struct snapshot *snapshot = calloc(1, sizeof(*snapshot));
    if (snapshot == NULL) {
        pthread_mutex_lock(&service->lock);
        service->progress.building = false;
        service->progress.last_error = TL_NOMEM;
        pthread_mutex_unlock(&service->lock);
        return TL_NOMEM;
    }
    snapshot->model = service->model;
    snapshot->model->references++;
    tl_status status = vec_create(sizeof(struct metadata), &snapshot->entries);
    if (status == TL_OK)
        status = copy_files(service, snapshot);
    if (status == TL_OK)
        status = copy_desktop(service, snapshot);
    const tl_emb_model *model = embedder_model(snapshot->model->embedder);
    if (status == TL_OK)
        status = vector_create_int8(model->emb_gen, model->dimensions, vec_count(snapshot->entries),
                                    service->options.vector_budget, &snapshot->vectors);
    if (status == TL_OK)
        status = stage_descriptor(service, model);
    if (status != TL_OK) {
        snapshot_free(snapshot);
        pthread_mutex_lock(&service->lock);
        service->progress.building = false;
        service->progress.last_error = status;
        pthread_mutex_unlock(&service->lock);
        return status;
    }
    service->stage = snapshot;
    return TL_OK;
}
static tl_status embed_row(tl_semantic *service, struct snapshot *snapshot) {
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
static void reclaim(tl_semantic *service) {
    pthread_mutex_lock(&service->lock);
    struct snapshot *garbage = service->retired;
    if (garbage != NULL && garbage->references == 0)
        service->retired = NULL;
    else
        garbage = NULL;
    pthread_mutex_unlock(&service->lock);
    snapshot_free(garbage);
}
static tl_status finish_stage(tl_semantic *service) {
    struct snapshot *snapshot = service->stage;
    tl_status status = vector_finish(snapshot->vectors);
    if (status == TL_OK)
        status = vector_workspace_create(snapshot->vectors, &snapshot->workspace);
    if (status == TL_OK)
        status = store_embedding_activate(service->store,
                                          embedder_model(snapshot->model->embedder)->emb_gen);
    if (status != TL_OK)
        return status;
    pthread_mutex_lock(&service->lock);
    service->retired = service->active;
    service->active = snapshot;
    service->stage = NULL;
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
    json_buffer_init(&buffer, out, capacity);
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
static void run_job(tl_semantic *service, struct job *job) {
    const tl_emb_model *model = embedder_model(job->snapshot->model->embedder);
    float query[VECTOR_MAX_DIMENSIONS];
    tl_status status = embedder_encode(job->snapshot->model->embedder, EMBED_QUERY,
                                       job->request.query, query, model->dimensions);
    tl_vector_result hits[SEMANTIC_RESULTS];
    size_t count = 0;
    if (status == TL_OK)
        status = vector_query(job->snapshot->vectors, job->snapshot->workspace, model->emb_gen,
                              query, model->dimensions, hits, SEMANTIC_RESULTS, &count);
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
        "ok", reason, job->output, IPC_RESPONSE_BYTES, &job->output_length);
    if (encoded != TL_OK) {
        encoded = encode_response(job, NULL, 0, "error", "response_limit", job->output,
                                  IPC_RESPONSE_BYTES, &job->output_length);
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
    snapshot_free(service->stage);
    service->stage = NULL;
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
        service->progress.total = vec_count(service->active->entries);
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
    if (service->stage != NULL && !current(service, service->stage)) {
        snapshot_free(service->stage);
        service->stage = NULL;
    }
    if (service->retired != NULL)
        return TL_OK; /* Publication waits instead of accumulating old views. */
    if (service->stage != NULL)
        return TL_OK;
    if (service->active != NULL && service->active->model == service->model &&
        current(service, service->active))
        return TL_OK;
    return begin_stage(service);
}
static void background(tl_semantic *service) {
    tl_status status = prepare_background(service);
    if (status != TL_OK || service->stage == NULL) {
        update_progress(service, status);
        return;
    }
    status = store_embedding_batch_begin(service->store);
    if (status == TL_OK) {
        for (size_t i = 0; i < BACKGROUND_BATCH && status == TL_OK &&
                           service->stage->position < vec_count(service->stage->entries);
             i++)
            status = embed_row(service, service->stage);
        tl_status committed = store_embedding_batch_end(service->store, status == TL_OK);
        if (committed != TL_OK)
            status = committed;
    }
    if (status == TL_OK && service->stage->position == vec_count(service->stage->entries))
        status = finish_stage(service);
    if (status != TL_OK) {
        snapshot_free(service->stage);
        service->stage = NULL;
    }
    update_progress(service, status);
}

static void *worker(void *context) {
    tl_semantic *service = context;
    while (!stopped(service)) {
        struct job *job = next_job(service);
        if (job != NULL) {
            run_job(service, job);
            continue;
        }
        background(service);
        if (service->stage != NULL)
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
    snapshot_free(service->stage);
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
        if (job->state == JOB_RUNNING)
            job->state = JOB_ABANDONED;
        else {
            job->snapshot->references--;
            job->state = JOB_FREE;
        }
    } else if (job->state == JOB_DONE) {
        if (job->output_length >= capacity)
            status = TL_LIMIT;
        else {
            memcpy(output, job->output, job->output_length);
            output[job->output_length] = 0;
            *out_length = job->output_length;
        }
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
        out->entries = vec_count(service->active->entries);
        out->vector_bytes = vector_bytes(service->active->vectors);
    }
    pthread_mutex_unlock(&service->lock);
    return TL_OK;
}
