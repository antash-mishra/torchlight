/* One cancellable discovery worker; immutable publication behind short leases. */
#include "torchlight/software_catalog.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#define SOFTWARE_CHECK_MS 15000
#define SOFTWARE_BACKSTOP_MS 600000
#define SOFTWARE_REFRESH_MS (6U * 60U * 60U * 1000U)
#define SOFTWARE_JOB_MS 60000
struct tl_software_catalog {
    pthread_mutex_t lock, wake_lock;
    pthread_cond_t wake;
    pthread_t thread;
    bool started, requested, refreshing;
    atomic_bool stop;
    tl_software_state state;
    tl_software_backend backend;
    tl_software *index;
    tl_software_reader *reader;
    uint64_t catalog_gen;
};
static void refresh(tl_software_catalog *catalog, bool download) {
    tl_software *index = NULL;
    tl_software_reader *reader = NULL;
    tl_process_budget budget = {.deadline_ms = process_now() + SOFTWARE_JOB_MS,
                                .remaining_bytes = SOFTWARE_TEXT_BYTES,
                                .cancel = &catalog->stop};
    tl_status status = catalog->backend.build(catalog->backend.context, download, &budget, &index);
    if (status == TL_OK)
        status = software_reader_create(index, &reader);
    pthread_mutex_lock(&catalog->lock);
    if (status == TL_OK || status == TL_STATE) {
        tl_software *old_index = catalog->index;
        tl_software_reader *old_reader = catalog->reader;
        catalog->index = index;
        catalog->reader = reader;
        if (catalog->catalog_gen != UINT64_MAX)
            catalog->catalog_gen++;
        index = old_index;
        reader = old_reader;
        catalog->state = status == TL_OK ? SOFTWARE_READY : SOFTWARE_UNAVAILABLE;
    } else
        catalog->state = catalog->index == NULL ? SOFTWARE_FAILED : SOFTWARE_STALE;
    pthread_mutex_unlock(&catalog->lock);
    software_reader_destroy(reader);
    software_destroy(index);
}
static void wait_check(tl_software_catalog *catalog) {
    uint64_t due = process_now() + SOFTWARE_CHECK_MS;
    struct timespec deadline = {.tv_sec = (time_t)(due / 1000),
                                .tv_nsec = (long)(due % 1000) * 1000000};
    pthread_mutex_lock(&catalog->wake_lock);
    if (!catalog->requested && !atomic_load(&catalog->stop))
        (void)pthread_cond_timedwait(&catalog->wake, &catalog->wake_lock, &deadline);
    pthread_mutex_unlock(&catalog->wake_lock);
}
static void *worker(void *context) {
    tl_software_catalog *catalog = context;
    tl_thread_name("tl-software");
    uint64_t stamp = 0, checked = 0, downloaded = 0;
    bool first = true, initial_download = true;
    while (!atomic_load(&catalog->stop)) {
        uint64_t now = process_now(), current = catalog->backend.stamp(catalog->backend.context);
        pthread_mutex_lock(&catalog->wake_lock);
        bool requested = catalog->requested;
        bool download =
            !first && (requested || initial_download || now - downloaded >= SOFTWARE_REFRESH_MS);
        bool run = first || requested || current != stamp ||
                   now - checked >= SOFTWARE_BACKSTOP_MS || download;
        catalog->requested = false;
        catalog->refreshing = run;
        pthread_mutex_unlock(&catalog->wake_lock);
        if (run) {
            refresh(catalog, download);
            stamp = current;
            checked = process_now();
            if (download) {
                downloaded = checked;
                initial_download = false;
            }
            /* A cached initial snapshot is usable while the next refresh fetches metadata. */
            if (first)
                downloaded = checked > SOFTWARE_REFRESH_MS ? checked - SOFTWARE_REFRESH_MS : 0;
            first = false;
        }
        pthread_mutex_lock(&catalog->wake_lock);
        catalog->refreshing = false;
        pthread_mutex_unlock(&catalog->wake_lock);
        wait_check(catalog);
    }
    return NULL;
}
tl_status software_catalog_create(bool enabled, const tl_software_backend *backend,
                                  tl_software_catalog **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_software_backend adapter = backend == NULL ? software_backend_flatpak() : *backend;
    if (adapter.build == NULL || adapter.stamp == NULL)
        return TL_INVALID;
    tl_software_catalog *catalog = calloc(1, sizeof(*catalog));
    if (catalog == NULL)
        return TL_NOMEM;
    catalog->backend = adapter;
    catalog->state = enabled ? SOFTWARE_LOADING : SOFTWARE_DISABLED;
    atomic_init(&catalog->stop, false);
    if (pthread_mutex_init(&catalog->lock, NULL) != 0) {
        free(catalog);
        return TL_IO;
    }
    if (pthread_mutex_init(&catalog->wake_lock, NULL) != 0) {
        pthread_mutex_destroy(&catalog->lock);
        free(catalog);
        return TL_IO;
    }
    pthread_condattr_t attr;
    int code = pthread_condattr_init(&attr);
    if (code == 0) {
        code = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        if (code == 0)
            code = pthread_cond_init(&catalog->wake, &attr);
        pthread_condattr_destroy(&attr);
    }
    if (code != 0) {
        pthread_mutex_destroy(&catalog->wake_lock);
        pthread_mutex_destroy(&catalog->lock);
        free(catalog);
        return TL_IO;
    }
    if (enabled) {
        code = pthread_create(&catalog->thread, NULL, worker, catalog);
        if (code != 0) {
            software_catalog_destroy(catalog);
            return TL_IO;
        }
        catalog->started = true;
    }
    *out = catalog;
    return TL_OK;
}
void software_catalog_destroy(tl_software_catalog *catalog) {
    if (catalog == NULL)
        return;
    atomic_store(&catalog->stop, true);
    pthread_mutex_lock(&catalog->wake_lock);
    pthread_cond_signal(&catalog->wake);
    pthread_mutex_unlock(&catalog->wake_lock);
    if (catalog->started)
        pthread_join(catalog->thread, NULL);
    software_reader_destroy(catalog->reader);
    software_destroy(catalog->index);
    pthread_cond_destroy(&catalog->wake);
    pthread_mutex_destroy(&catalog->wake_lock);
    pthread_mutex_destroy(&catalog->lock);
    free(catalog);
}
void software_catalog_acquire(tl_software_catalog *catalog) {
    if (catalog != NULL)
        pthread_mutex_lock(&catalog->lock);
}
void software_catalog_release(tl_software_catalog *catalog) {
    if (catalog != NULL)
        pthread_mutex_unlock(&catalog->lock);
}
tl_status software_catalog_query(tl_software_catalog *catalog, const char *query,
                                 const atomic_bool *cancel, const tl_software_entry **out,
                                 size_t capacity, size_t *count) {
    if (count == NULL)
        return TL_INVALID;
    *count = 0;
    if (catalog == NULL || query == NULL || out == NULL || capacity == 0 ||
        capacity > LEXICAL_MAX_RESULTS)
        return TL_INVALID;
    return catalog->reader == NULL
               ? TL_OK
               : software_query(catalog->reader, query, cancel, out, capacity, count);
}
tl_status software_catalog_suggest(tl_software_catalog *catalog, const char *query,
                                   const atomic_bool *cancel, const tl_software_entry **out,
                                   size_t capacity, size_t *count, bool *first_exact) {
    if (count == NULL || first_exact == NULL)
        return TL_INVALID;
    *count = 0;
    *first_exact = false;
    if (catalog == NULL || query == NULL || out == NULL || capacity == 0 ||
        capacity > SOFTWARE_SUGGESTIONS)
        return TL_INVALID;
    return catalog->reader == NULL ? TL_OK
                                   : software_suggest(catalog->reader, query, cancel, out, capacity,
                                                      count, first_exact);
}
const tl_software_entry *software_catalog_resolve(const tl_software_catalog *catalog,
                                                  const tl_software_ref *ref, uint64_t revision) {
    return catalog == NULL ? NULL : software_resolve(catalog->index, ref, revision);
}
tl_software_state software_catalog_state(const tl_software_catalog *catalog) {
    return catalog == NULL ? SOFTWARE_DISABLED : catalog->state;
}
size_t software_catalog_count(const tl_software_catalog *catalog) {
    return catalog == NULL ? 0 : software_count(catalog->index);
}
uint64_t software_catalog_gen(const tl_software_catalog *catalog) {
    return catalog == NULL ? 0 : catalog->catalog_gen;
}
const char *software_catalog_state_name(tl_software_state state) {
    static const char *const names[] = {"disabled",    "loading", "ready",
                                        "unavailable", "failed",  "stale"};
    return (unsigned)state < sizeof(names) / sizeof(names[0]) ? names[state] : "unknown";
}
void software_catalog_refresh(tl_software_catalog *catalog) {
    if (catalog == NULL || !catalog->started)
        return;
    pthread_mutex_lock(&catalog->wake_lock);
    if (!catalog->refreshing)
        catalog->requested = true;
    pthread_cond_signal(&catalog->wake);
    pthread_mutex_unlock(&catalog->wake_lock);
}
