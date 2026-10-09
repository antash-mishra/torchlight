/* Resident two-phase query service. An IPC thread owns sockets and framing; a
 * search thread owns catalog leases, scoring and encoding. Each client keeps a
 * bounded request queue whose newest query supersedes older queued queries and
 * cancels a running one. Sockets never retain a lexical workspace lease. */
#include "torchlight/daemon.h"
#include "torchlight/desktop.h"
#include "torchlight/personal.h"
#include "torchlight/semantic.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
/* History retention is configured in days; the usage summary takes seconds. */
#define DAEMON_SECONDS_PER_DAY 86400
enum pending_state { PENDING_QUEUED, PENDING_RUNNING };
/* One decoded request waiting for, or being served by, the search thread. */
struct pending {
    tl_ipc_request request;
    tl_status decoded;
    bool repeated, superseded;
    enum pending_state state;
};
struct client {
    tl_semantic *semantic;
    size_t slot;
    uint64_t semantic_token;
    bool semantic_pending;
    /* The search thread submitted the pending job but has not yet queued the
     * lexical frame it follows, so its final must wait (a fast worker would
     * otherwise overtake a large frame being encoded outside the lock). */
    bool lexical_inflight;
    /* A newer query cancelled the pending job while lexical_inflight; the
     * cancellation is taken once that frame is queued. */
    bool cancel_semantic;
    char pending_id[IPC_REQUEST_ID_BYTES + 1];
    int fd;
    char input[IPC_REQUEST_BYTES];
    size_t input_length;
    char *output;
    size_t output_length, sent;
    char active[DAEMON_ACTIVE_REQUESTS][IPC_REQUEST_ID_BYTES + 1];
    size_t active_count;
    bool closing, abort;
    uint64_t deadline;
    /* FIFO shared with the search thread under the daemon lock. At most one
     * entry (the head) is running at any time. */
    struct pending queue[DAEMON_ACTIVE_REQUESTS];
    size_t queue_head, queue_count;
    /* Advances on close so a completion for a reused slot is discarded. */
    uint64_t generation;
};
struct tl_daemon {
    tl_catalog *catalog;
    tl_desktop *desktop;
    tl_writer *writer;
    tl_semantic *semantic;
    tl_ipc_listener *listener;
    int signal_fd, database_lock, done_fd;
    sigset_t old_mask;
    bool masked;
    char session[33];
    uint64_t search_sequence;
    uint64_t engine_ns;
    /* Search-thread state: lock/wake guard every client's queue, output
     * accounting and semantic bookkeeping; cancel targets the running query. */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t search_thread;
    bool search_started, lock_ready;
    atomic_bool stop, cancel;
    size_t next_client;
    char *scratch;
    /* The job the search thread is serving; copied out of its client's queue. */
    struct pending job;
    /* M5 personal ranking, search thread only (ADR 0033): NULL with history
     * disabled; usage_loading until the writer offers the startup summary;
     * the current query's desktop boosts. */
    tl_personal *personal;
    bool usage_loading;
    const tl_lexical_boost *app_boosts;
    size_t app_boost_count;
    struct client clients[DAEMON_MAX_CLIENTS];
    tl_result results[LEXICAL_MAX_RESULTS];
    tl_result applications[LEXICAL_MAX_RESULTS];
};
static uint64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static uint64_t nanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static void lock_daemon(tl_daemon *daemon) {
    int code = pthread_mutex_lock(&daemon->lock);
    (void)code;
}
static void unlock_daemon(tl_daemon *daemon) {
    int code = pthread_mutex_unlock(&daemon->lock);
    (void)code;
}
static tl_status session_id(char out[33]) {
    unsigned char bytes[16];
    size_t offset = 0;
    while (offset < sizeof(bytes)) {
        ssize_t count = getrandom(bytes + offset, sizeof(bytes) - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return TL_IO;
        offset += (size_t)count;
    }
    static const char HEX[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); i++) {
        out[i * 2] = HEX[bytes[i] >> 4];
        out[i * 2 + 1] = HEX[bytes[i] & 15];
    }
    out[32] = 0;
    return TL_OK;
}
static void discard_semantic(struct client *client) {
    if (client->semantic_pending) {
        char discarded[1024];
        size_t length = 0;
        tl_status status = semantic_take(client->semantic, client->slot, client->semantic_token,
                                         true, discarded, sizeof(discarded), &length);
        (void)status;
        client->semantic_pending = false;
    }
    client->lexical_inflight = false;
    client->cancel_semantic = false;
}
/* IPC thread only, under the daemon lock. A running search for this client
 * is cancelled and its completion dropped through the generation check. */
static void close_client(tl_daemon *daemon, struct client *client) {
    discard_semantic(client);
    if (client->fd >= 0)
        close(client->fd);
    if (client->queue_count != 0 && client->queue[client->queue_head].state == PENDING_RUNNING)
        atomic_store(&daemon->cancel, true);
    client->fd = -1;
    client->input_length = 0;
    client->output_length = 0;
    client->sent = 0;
    client->active_count = 0;
    client->closing = false;
    client->abort = false;
    client->queue_head = 0;
    client->queue_count = 0;
    client->generation++;
}
static tl_status signals(tl_daemon *daemon) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &mask, &daemon->old_mask) != 0)
        return TL_IO;
    daemon->masked = true;
    daemon->signal_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    return daemon->signal_fd < 0 ? TL_IO : TL_OK;
}
static void *search_worker(void *context);
static tl_status create_services(tl_daemon *daemon, const tl_daemon_options *options) {
    tl_writer_options writer_options = {.config = options->config,
                                        .catalog = daemon->catalog,
                                        .socket_path = options->socket_path,
                                        .watch_capacity = options->watch_capacity,
                                        .max_entries = options->max_entries,
                                        .max_path_bytes = options->max_path_bytes,
                                        .rescan_ms = options->rescan_ms,
                                        .history_days = options->history_days,
                                        .history = options->history,
                                        .readers = 1};
    tl_status status = writer_create(&writer_options, &daemon->writer);
    if (status == TL_OK)
        status = desktop_create(&daemon->desktop);
    if (status == TL_OK && options->model_path != NULL) {
        tl_semantic_options semantic_options = {.model_path = options->model_path,
                                                .database = config_database(options->config),
                                                .catalog = daemon->catalog,
                                                .desktop = daemon->desktop,
                                                .vector_budget = SEMANTIC_VECTOR_BYTES,
                                                .metadata_budget = 256U * 1024U * 1024U,
                                                .deadline_ms = options->semantic_deadline_ms == 0
                                                                   ? SEMANTIC_DEADLINE_MS
                                                                   : options->semantic_deadline_ms};
        status = semantic_create(&semantic_options, &daemon->semantic);
        for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++) {
            daemon->clients[i].semantic = daemon->semantic;
            daemon->clients[i].slot = i;
        }
    }
    if (status == TL_OK && options->history) {
        status = personal_create((int64_t)options->history_days * DAEMON_SECONDS_PER_DAY,
                                 &daemon->personal);
        daemon->usage_loading = status == TL_OK;
    }
    if (status == TL_OK)
        status = ipc_listener_create(options->socket_path, &daemon->listener);
    return status;
}
tl_status daemon_create(const tl_daemon_options *options, tl_daemon **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (options == NULL || options->config == NULL || options->socket_path == NULL)
        return TL_INVALID;
    tl_daemon *daemon = calloc(1, sizeof(*daemon));
    if (daemon == NULL)
        return TL_NOMEM;
    daemon->signal_fd = -1;
    daemon->database_lock = -1;
    daemon->done_fd = -1;
    atomic_init(&daemon->stop, false);
    atomic_init(&daemon->cancel, false);
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++)
        daemon->clients[i].fd = -1;
    tl_status status = TL_OK;
    if (pthread_mutex_init(&daemon->lock, NULL) != 0 || pthread_cond_init(&daemon->wake, NULL) != 0)
        status = TL_IO;
    else
        daemon->lock_ready = true;
    if (status == TL_OK)
        status = ipc_database_lock(config_database(options->config), &daemon->database_lock);
    if (status == TL_OK)
        status = signals(daemon);
    if (status == TL_OK)
        status = session_id(daemon->session);
    if (status == TL_OK)
        status = catalog_create(2, &daemon->catalog);
    if (status == TL_OK) {
        daemon->done_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        daemon->scratch = malloc(IPC_RESPONSE_BYTES);
        if (daemon->done_fd < 0)
            status = TL_IO;
        else if (daemon->scratch == NULL)
            status = TL_NOMEM;
    }
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS && status == TL_OK; i++) {
        daemon->clients[i].output = malloc(IPC_RESPONSE_BYTES);
        if (daemon->clients[i].output == NULL)
            status = TL_NOMEM;
    }
    if (status == TL_OK)
        status = create_services(daemon, options);
    if (status == TL_OK && pthread_create(&daemon->search_thread, NULL, search_worker, daemon) != 0)
        status = TL_IO;
    if (status != TL_OK) {
        tl_status cleanup = daemon_destroy(daemon);
        if (cleanup != TL_OK)
            return cleanup;
        return status;
    }
    daemon->search_started = true;
    *out = daemon;
    return TL_OK;
}
static void stop_search(tl_daemon *daemon) {
    atomic_store(&daemon->stop, true);
    atomic_store(&daemon->cancel, true);
    if (!daemon->search_started)
        return;
    lock_daemon(daemon);
    int code = pthread_cond_broadcast(&daemon->wake);
    unlock_daemon(daemon);
    (void)code;
    code = pthread_join(daemon->search_thread, NULL);
    (void)code;
    daemon->search_started = false;
}
tl_status daemon_destroy(tl_daemon *daemon) {
    if (daemon == NULL)
        return TL_OK;
    if (daemon->lock_ready)
        stop_search(daemon);
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++) {
        close_client(daemon, &daemon->clients[i]);
        free(daemon->clients[i].output);
        daemon->clients[i].output = NULL;
    }
    ipc_listener_destroy(daemon->listener);
    daemon->listener = NULL;
    personal_destroy(daemon->personal);
    daemon->personal = NULL;
    semantic_destroy(daemon->semantic);
    daemon->semantic = NULL;
    desktop_destroy(daemon->desktop);
    daemon->desktop = NULL;
    writer_destroy(daemon->writer);
    daemon->writer = NULL;
    tl_status status = catalog_destroy(daemon->catalog);
    if (status != TL_OK)
        return status;
    daemon->catalog = NULL;
    if (daemon->done_fd >= 0)
        close(daemon->done_fd);
    free(daemon->scratch);
    if (daemon->signal_fd >= 0)
        close(daemon->signal_fd);
    if (daemon->masked) {
        int code = pthread_sigmask(SIG_SETMASK, &daemon->old_mask, NULL);
        (void)code;
    }
    if (daemon->lock_ready) {
        int code = pthread_mutex_destroy(&daemon->lock);
        (void)code;
        code = pthread_cond_destroy(&daemon->wake);
        (void)code;
    }
    ipc_unlock(daemon->database_lock);
    free(daemon);
    return TL_OK;
}
static void boolean(tl_json_buffer *buffer, bool value) {
    json_raw(buffer, value ? "true" : "false");
}
static void indexing_status(tl_json_buffer *b, const tl_writer_stats *stats,
                            const tl_semantic_stats *semantic, size_t personal_items) {
    json_raw(b, ",\"indexing\":{\"active\":");
    boolean(b, stats->indexing);
    json_raw(b, ",\"degraded\":");
    boolean(b, stats->degraded);
    json_raw(b, ",\"recovering\":");
    boolean(b, stats->recovering);
    json_raw(b, ",\"watch_degraded\":");
    boolean(b, stats->watch_degraded);
    json_raw(b, ",\"reconciliations\":");
    json_number(b, stats->reconciliations);
    json_raw(b, ",\"scoped_reconciliations\":");
    json_number(b, stats->scoped_reconciliations);
    json_raw(b, ",\"delta_publications\":");
    json_number(b, stats->delta_publications);
    json_raw(b, ",\"full_builds\":");
    json_number(b, stats->full_builds);
    json_raw(b, ",\"watches\":");
    json_number(b, stats->watches);
    json_raw(b, ",\"watch_overflows\":");
    json_number(b, stats->watch_overflows);
    json_raw(b, ",\"watch_unavailable\":");
    json_number(b, stats->watch_unavailable);
    json_raw(b, ",\"last_scan_ms\":");
    json_number(b, stats->last_scan_ms);
    json_raw(b, ",\"offline_roots\":");
    json_number(b, stats->offline_roots);
    json_raw(b, ",\"unreadable_scopes\":");
    json_number(b, stats->unreadable_scopes);
    json_raw(b, "},\"history\":{\"enabled\":");
    boolean(b, stats->history_enabled);
    json_raw(b, ",\"pending\":");
    json_number(b, stats->history_pending);
    json_raw(b, ",\"dropped\":");
    json_number(b, stats->history_dropped);
    json_raw(b, ",\"failures\":");
    json_number(b, stats->history_failures);
    json_raw(b, ",\"written\":");
    json_number(b, stats->history_written);
    json_raw(b, ",\"personal_items\":");
    json_number(b, personal_items);
    json_raw(b, "}");
    if (semantic != NULL) {
        json_raw(b, ",\"semantic\":{\"available\":");
        boolean(b, semantic->available);
        json_raw(b, ",\"building\":");
        boolean(b, semantic->building);
        json_raw(b, ",\"processed\":");
        json_number(b, semantic->processed);
        json_raw(b, ",\"total\":");
        json_number(b, semantic->total);
        json_raw(b, ",\"reused\":");
        json_number(b, semantic->reused);
        json_raw(b, ",\"derived_stages\":");
        json_number(b, semantic->derived_stages);
        json_raw(b, ",\"full_stages\":");
        json_number(b, semantic->full_stages);
        json_raw(b, ",\"vector_bytes\":");
        json_number(b, semantic->vector_bytes);
        json_raw(b, ",\"last_error\":");
        json_quote(b, tl_status_string(semantic->last_error));
        json_raw(b, "}");
    }
}
static void response_prefix(tl_json_buffer *b, const tl_ipc_request *request, uint64_t catalog_gen,
                            const char *search_id, const char *status, const char *reason,
                            const tl_writer_stats *stats, bool lexical_phase, uint64_t emb_gen,
                            const tl_semantic_stats *semantic, size_t personal_items) {
    json_raw(b, "{\"version\":1,\"request_id\":");
    json_quote(b, request->request_id);
    json_raw(b, ",\"phase\":");
    json_quote(b, lexical_phase ? "lexical" : "final");
    json_raw(b, ",\"catalog_gen\":");
    json_number(b, catalog_gen);
    json_raw(b, ",\"emb_gen\":");
    if (emb_gen == 0)
        json_raw(b, "null");
    else
        json_number(b, emb_gen);
    json_raw(b, ",\"search_id\":");
    if (search_id[0] == 0)
        json_raw(b, "null");
    else
        json_quote(b, search_id);
    json_raw(b, ",\"status\":");
    json_quote(b, status);
    json_raw(b, ",\"reason\":");
    json_quote(b, reason);
    indexing_status(b, stats, semantic, personal_items);
    json_raw(b, ",\"results\":[");
}
static tl_status merge_applications(tl_daemon *daemon, const tl_ipc_request *request,
                                    size_t *count) {
    size_t app_count = 0;
    tl_status status = desktop_query_boosted(daemon->desktop, request->query, daemon->app_boosts,
                                             daemon->app_boost_count, daemon->applications,
                                             request->limit, &app_count);
    if (status != TL_OK)
        return status;
    for (size_t i = 0; i < app_count; i++) {
        tl_result candidate = daemon->applications[i];
        size_t place = 0;
        /* Preserve the desktop engine's order at ties, so limit=1 and limit=10
         * expose the same head. Desktop results still precede tied files. */
        while (place < *count && (daemon->results[place].score > candidate.score ||
                                  (daemon->results[place].score == candidate.score &&
                                   daemon->results[place].id >= DESKTOP_ID_BASE)))
            place++;
        if (place >= request->limit)
            continue;
        size_t end = *count < request->limit ? (*count)++ : request->limit - 1;
        for (size_t j = end; j > place; j--)
            daemon->results[j] = daemon->results[j - 1];
        daemon->results[place] = candidate;
    }
    return TL_OK;
}
/* Search thread: queue an accepted open for saving, with the query of its
 * search when still remembered (saved with it), and count it in the live
 * usage summary, even if the bounded queue had to drop saving it. */
static tl_status record_open(tl_daemon *daemon, tl_ipc_request *event, tl_usage_target target) {
    const char *query = personal_query_of(daemon->personal, event->search_id);
    if (query == NULL)
        event->query[0] = 0;
    else
        memcpy(event->query, query, strlen(query) + 1);
    tl_status status = writer_history(daemon->writer, event, NULL);
    if (status == TL_STATE)
        return TL_OK; /* history disabled */
    /* TL_LIMIT: the history queue is full, so this open is not saved (and is
     * counted as dropped); the launch itself succeeded, so it still counts. */
    if (status == TL_OK || status == TL_LIMIT) {
        /* Ranking is only a hint and never fails an open: the one rejection,
         * a desktop id too long to keep, just leaves that app unboosted. */
        tl_status counted =
            personal_record(daemon->personal, event->event_id, target, query, (int64_t)time(NULL));
        (void)counted;
    }
    return status;
}
/* Search thread: this query's boosts, or none without history. Personal
 * ranking never fails a search: on error the query runs unboosted. */
static void prepare_boosts(tl_daemon *daemon, const char *query, tl_catalog_boosts *files) {
    *files = (tl_catalog_boosts){0};
    daemon->app_boosts = NULL;
    daemon->app_boost_count = 0;
    if (daemon->personal == NULL)
        return;
    if (personal_boosts(daemon->personal, daemon->desktop, query, (int64_t)time(NULL), files,
                        &daemon->app_boosts, &daemon->app_boost_count) != TL_OK) {
        *files = (tl_catalog_boosts){0};
        daemon->app_boosts = NULL;
        daemon->app_boost_count = 0;
    }
}
/* Search thread: adopt the startup usage summary once the writer offers it. */
static void adopt_usage(tl_daemon *daemon) {
    tl_usage *loaded = NULL;
    if (!daemon->usage_loading || !writer_take_usage(daemon->writer, &loaded))
        return;
    daemon->usage_loading = false;
    personal_adopt(daemon->personal, loaded);
}
static tl_status resolve_application(tl_daemon *daemon, const tl_ipc_request *request,
                                     size_t *count) {
    const tl_desktop_entry *entry = desktop_resolve(daemon->desktop, request->file_id);
    if (entry == NULL)
        return TL_STATE;
    daemon->results[0] = (tl_result){entry->id, entry->filename, 0};
    *count = 1;
    if (request->operation != IPC_OPEN)
        return TL_OK;
    tl_ipc_request event = *request;
    size_t length = strlen(entry->desktop_id);
    if (length >= sizeof(event.desktop_id))
        return TL_LIMIT;
    memcpy(event.desktop_id, entry->desktop_id, length + 1);
    return record_open(daemon, &event, (tl_usage_target){0, entry->desktop_id});
}
/* Search thread only: run the query/resolve/open against the leased view. */
static tl_status execute_leased(tl_daemon *daemon, const tl_ipc_request *request, size_t *count,
                                uint64_t *catalog_gen, tl_catalog_reader **reader) {
    tl_status status = catalog_acquire(daemon->catalog, reader);
    if (status != TL_OK)
        return status;
    *catalog_gen = catalog_reader_gen(*reader);
    if (request->operation == IPC_QUERY) {
        catalog_reader_cancel(*reader, &daemon->cancel);
        uint64_t start = nanoseconds();
        tl_catalog_boosts files;
        prepare_boosts(daemon, request->query, &files);
        status = catalog_query_boosted(*reader, request->query, &files, daemon->results,
                                       request->limit, count);
        if (status == TL_OK)
            status = merge_applications(daemon, request, count);
        uint64_t end = nanoseconds();
        daemon->engine_ns = end >= start ? end - start : 0;
        return status;
    }
    const char *path = NULL;
    status = catalog_resolve(*reader, request->file_id, &path);
    if (status != TL_OK)
        return status;
    daemon->results[0] = (tl_result){request->file_id, path, 0};
    *count = 1;
    if (request->operation == IPC_OPEN) {
        tl_ipc_request event = *request;
        status = record_open(daemon, &event, (tl_usage_target){request->file_id, NULL});
    }
    return status;
}
static tl_status execute(tl_daemon *daemon, const tl_ipc_request *request,
                         char search_id[IPC_HISTORY_ID_BYTES + 1], size_t *count,
                         uint64_t *catalog_gen, tl_catalog_reader **reader, bool superseded) {
    *count = 0;
    if (request->operation == IPC_QUERY) {
        if (daemon->search_sequence == UINT64_MAX)
            return TL_LIMIT;
        int length = snprintf(search_id, IPC_HISTORY_ID_BYTES + 1, "%s:%llu", daemon->session,
                              (unsigned long long)++daemon->search_sequence);
        if (length < 0 || (size_t)length > IPC_HISTORY_ID_BYTES)
            return TL_LIMIT;
        /* A search is saved only with an open that references it (ADR 0033):
         * remember its query instead of queueing a write per keystroke. */
        if (superseded)
            return TL_STATE;
        personal_remember(daemon->personal, search_id, request->query);
    }
    if ((request->operation == IPC_RESOLVE || request->operation == IPC_OPEN) &&
        request->file_id >= DESKTOP_ID_BASE)
        return resolve_application(daemon, request, count);
    if (request->operation == IPC_QUERY || request->operation == IPC_RESOLVE ||
        request->operation == IPC_OPEN)
        return execute_leased(daemon, request, count, catalog_gen, reader);
    if (request->operation == IPC_RECONCILE) {
        desktop_refresh(daemon->desktop);
        return writer_reconcile(daemon->writer);
    }
    if (request->operation == IPC_HISTORY_CLEAR) {
        personal_clear(daemon->personal);
        return writer_history(daemon->writer, request, NULL);
    }
    return TL_OK;
}
/* Under the daemon lock: ids answered but unsent, pending semantic, or queued. */
static bool duplicate(const struct client *client, const char *id) {
    for (size_t i = 0; i < client->active_count; i++)
        if (strcmp(client->active[i], id) == 0)
            return true;
    for (size_t i = 0; i < client->queue_count; i++) {
        size_t index = (client->queue_head + i) % DAEMON_ACTIVE_REQUESTS;
        if (strcmp(client->queue[index].request.request_id, id) == 0)
            return true;
    }
    return false;
}
static const char *response_reason(const tl_ipc_request *request, tl_status status,
                                   tl_status decoded, bool repeated, bool superseded) {
    if (repeated)
        return "duplicate_active_request_id";
    if (decoded != TL_OK)
        return "invalid_request";
    if (superseded)
        return "superseded";
    if (status == TL_STATE && (request->operation == IPC_RESOLVE || request->operation == IPC_OPEN))
        return "stale_result";
    if (status != TL_OK)
        return tl_status_string(status);
    return request->operation == IPC_QUERY ? "lexical_only" : "accepted";
}
struct completion {
    const tl_ipc_request *request;
    tl_writer_stats stats;
    uint64_t catalog_gen;
    char search_id[IPC_HISTORY_ID_BYTES + 1];
    size_t count;
    tl_status status;
    const char *reason;
    bool superseded;
    tl_catalog_reader *reader;
    bool lexical_phase;
    uint64_t emb_gen;
    tl_semantic_stats semantic;
};
static void encode_result(tl_daemon *daemon, tl_catalog_reader *reader, tl_json_buffer *buffer,
                          const tl_result *result) {
    const tl_desktop_entry *entry = desktop_resolve(daemon->desktop, result->id);
    if (entry == NULL) {
        ipc_result(buffer, result->id, result->path);
        if (buffer->status == TL_OK) {
            buffer->data[--buffer->length] = 0;
            json_raw(buffer, ",\"kind\":");
            json_quote(buffer, catalog_is_dir(reader, result->id) ? "folder" : "file");
            json_raw(buffer, "}");
        }
        return;
    }
    ipc_result(buffer, entry->id, entry->filename);
    /* Extend the result object without changing exact byte-path encoding. */
    if (buffer->status != TL_OK)
        return;
    buffer->length--;
    buffer->data[buffer->length] = 0;
    json_raw(buffer, ",\"kind\":");
    json_quote(buffer, entry->settings ? "settings" : "application");
    json_raw(buffer, ",\"desktop_revision\":\"");
    json_number(buffer, entry->revision);
    json_raw(buffer, "\",\"name\":");
    json_quote(buffer, entry->name);
    json_raw(buffer, ",\"desktop_id\":");
    json_quote(buffer, entry->desktop_id);
    json_raw(buffer, ",\"icon\":");
    json_quote(buffer, entry->icon == NULL ? "application-x-executable-symbolic" : entry->icon);
    if (entry->wm_class != NULL) {
        json_raw(buffer, ",\"wm_class\":");
        json_quote(buffer, entry->wm_class);
    }
    json_raw(buffer, "}");
}
/* Encode one frame into the search thread's scratch. A frame that cannot fit
 * the whole buffer becomes a terminal response_limit error (overflow set). */
static tl_status encode_completion(tl_daemon *daemon, const struct completion *completion,
                                   size_t *length, bool *overflow) {
    const char *status = completion->status == TL_OK ? "ok"
                         : completion->superseded    ? "cancelled"
                                                     : "error";
    const tl_semantic_stats *semantic = daemon->semantic == NULL ? NULL : &completion->semantic;
    tl_json_buffer b;
    *overflow = false;
    json_buffer_init(&b, daemon->scratch, IPC_RESPONSE_BYTES);
    size_t items = personal_items(daemon->personal);
    response_prefix(&b, completion->request, completion->catalog_gen, completion->search_id, status,
                    completion->reason, &completion->stats, completion->lexical_phase,
                    completion->emb_gen, semantic, items);
    for (size_t i = 0;
         i < completion->count && i < completion->request->limit && completion->status == TL_OK;
         i++) {
        if (i != 0)
            json_raw(&b, ",");
        encode_result(daemon, completion->reader, &b, &daemon->results[i]);
    }
    json_raw(&b, "],\"timing\":{\"engine_us\":");
    json_number(&b, daemon->engine_ns / 1000);
    json_raw(&b, "}}\n");
    if (b.status != TL_OK) {
        *overflow = true;
        json_buffer_init(&b, daemon->scratch, IPC_RESPONSE_BYTES);
        response_prefix(&b, completion->request, completion->catalog_gen, completion->search_id,
                        "error", "response_limit", &completion->stats, false, completion->emb_gen,
                        semantic, items);
        json_raw(&b, "]}\n");
    }
    *length = b.status == TL_OK ? b.length : 0;
    return b.status;
}
static bool take_final(tl_daemon *daemon, struct client *client, bool cancel);
/* Under the daemon lock, before encoding: the lexical frame's reason depends on
 * whether the semantic worker accepted the job. */
static void queue_semantic(tl_daemon *daemon, struct client *client,
                           struct completion *completion) {
    const tl_ipc_request *request = completion->request;
    if (daemon->semantic == NULL || request->operation != IPC_QUERY ||
        completion->status != TL_OK || completion->superseded ||
        client->semantic_token == UINT64_MAX)
        return;
    /* A job still pending here belongs to an older query whose cancellation
     * waited for its lexical frame; take it now so the slot is free. */
    if (!take_final(daemon, client, true)) {
        client->abort = true;
        return;
    }
    tl_status queued = semantic_submit(daemon->semantic, client->slot, ++client->semantic_token,
                                       request, completion->search_id, completion->catalog_gen,
                                       desktop_gen(daemon->desktop), daemon->results,
                                       completion->count, &completion->emb_gen);
    if (queued == TL_OK) {
        completion->lexical_phase = true;
        completion->reason = "semantic_pending";
        client->semantic_pending = true;
        client->lexical_inflight = true;
        memcpy(client->pending_id, request->request_id, strlen(request->request_id) + 1);
    } else {
        completion->reason = queued == TL_LIMIT ? "semantic_queue_full" : "semantic_unavailable";
    }
}
static void wake_ipc(tl_daemon *daemon) {
    uint64_t one = 1;
    ssize_t count = write(daemon->done_fd, &one, sizeof(one));
    (void)count;
}
/* Under the daemon lock: hand the encoded frame to the client's output queue
 * and retire the served queue head. Dropped silently once the client closed. */
static void deliver(tl_daemon *daemon, struct client *client, const struct pending *job,
                    uint64_t generation, size_t length, bool overflow) {
    if (client->generation != generation || client->fd < 0)
        return;
    client->lexical_inflight = false;
    if (overflow)
        discard_semantic(client);
    if (length == 0 || IPC_RESPONSE_BYTES - client->output_length < length) {
        client->abort = true;
        return;
    }
    memcpy(client->output + client->output_length, daemon->scratch, length);
    client->output_length += length;
    if (client->active_count < DAEMON_ACTIVE_REQUESTS)
        memcpy(client->active[client->active_count++], job->request.request_id,
               strlen(job->request.request_id) + 1);
    if (job->decoded != TL_OK || job->repeated)
        client->closing = true;
    client->queue_head = (client->queue_head + 1) % DAEMON_ACTIVE_REQUESTS;
    client->queue_count--;
}
static void serve(tl_daemon *daemon, struct client *client, const struct pending *job,
                  uint64_t generation) {
    struct completion completion = {.request = &job->request, .superseded = job->superseded};
    tl_catalog_stats catalog = {0};
    completion.status = writer_stats(daemon->writer, &completion.stats);
    if (completion.status == TL_OK && daemon->semantic != NULL) {
        completion.status = semantic_stats(daemon->semantic, &completion.semantic);
        if (job->request.operation == IPC_STATUS)
            completion.emb_gen = completion.semantic.emb_gen;
    }
    if (completion.status == TL_OK)
        completion.status = catalog_stats(daemon->catalog, &catalog);
    if (job->decoded != TL_OK)
        completion.status = job->decoded;
    if (job->repeated)
        completion.status = TL_INVALID;
    completion.catalog_gen = catalog.catalog_gen;
    tl_catalog_reader *reader = NULL;
    daemon->engine_ns = 0;
    adopt_usage(daemon);
    desktop_acquire(daemon->desktop);
    tl_ipc_request pooled = job->request;
    if (daemon->semantic != NULL && job->request.operation == IPC_QUERY)
        pooled.limit = LEXICAL_MAX_RESULTS;
    if (completion.status == TL_OK)
        completion.status = execute(daemon, &pooled, completion.search_id, &completion.count,
                                    &completion.catalog_gen, &reader, job->superseded);
    if (completion.status == TL_CANCELLED) {
        /* A newer query arrived mid-search: answer like a queued supersession. */
        completion.status = TL_STATE;
        completion.superseded = true;
    }
    completion.reader = reader;
    completion.reason = response_reason(&job->request, completion.status, job->decoded,
                                        job->repeated, completion.superseded);
    lock_daemon(daemon);
    if (client->generation == generation && client->fd >= 0)
        queue_semantic(daemon, client, &completion);
    unlock_daemon(daemon);
    size_t length = 0;
    bool overflow = false;
    tl_status encoded = encode_completion(daemon, &completion, &length, &overflow);
    catalog_release(reader);
    desktop_release(daemon->desktop);
    lock_daemon(daemon);
    deliver(daemon, client, job, generation, encoded == TL_OK ? length : 0, overflow);
    unlock_daemon(daemon);
    wake_ipc(daemon);
}
/* Under the daemon lock: claim the next queued head, rotating across clients
 * so one busy connection cannot starve the others. */
static bool take_job(tl_daemon *daemon, struct client **out, struct pending *job,
                     uint64_t *generation) {
    for (size_t n = 0; n < DAEMON_MAX_CLIENTS; n++) {
        size_t i = (daemon->next_client + n) % DAEMON_MAX_CLIENTS;
        struct client *client = &daemon->clients[i];
        if (client->fd < 0 || client->queue_count == 0)
            continue;
        struct pending *head = &client->queue[client->queue_head];
        head->state = PENDING_RUNNING;
        *job = *head;
        *out = client;
        *generation = client->generation;
        atomic_store(&daemon->cancel, false);
        daemon->next_client = (i + 1) % DAEMON_MAX_CLIENTS;
        return true;
    }
    return false;
}
static void *search_worker(void *context) {
    tl_daemon *daemon = context;
    lock_daemon(daemon);
    while (!atomic_load(&daemon->stop)) {
        struct client *client = NULL;
        uint64_t generation = 0;
        if (!take_job(daemon, &client, &daemon->job, &generation)) {
            int code = pthread_cond_wait(&daemon->wake, &daemon->lock);
            (void)code;
            continue;
        }
        unlock_daemon(daemon);
        serve(daemon, client, &daemon->job, generation);
        lock_daemon(daemon);
    }
    unlock_daemon(daemon);
    return NULL;
}
static tl_status append_semantic_status(tl_daemon *daemon, char *output, size_t capacity,
                                        size_t *length) {
    tl_writer_stats writer;
    tl_semantic_stats semantic;
    tl_status status = writer_stats(daemon->writer, &writer);
    if (status == TL_OK)
        status = semantic_stats(daemon->semantic, &semantic);
    if (status != TL_OK)
        return status;
    if (*length < 2 || output[*length - 2] != '}' || output[*length - 1] != '\n')
        return TL_STATE;
    /* Extend the worker's owned envelope with the coordinator's current status. */
    size_t prefix = *length - 2;
    tl_json_buffer buffer;
    json_buffer_init(&buffer, output + prefix, capacity - prefix);
    indexing_status(&buffer, &writer, &semantic, personal_items(daemon->personal));
    json_raw(&buffer, "}\n");
    *length = buffer.status == TL_OK ? prefix + buffer.length : 0;
    return buffer.status;
}
/* Under the daemon lock (either thread): append the pending final, or with
 * cancel its cancellation, to the output queue once it may follow the frames
 * already queued. Returns false when the client must be closed. */
static bool take_final(tl_daemon *daemon, struct client *client, bool cancel) {
    cancel = cancel || client->cancel_semantic;
    if (!client->semantic_pending)
        return true;
    if (client->lexical_inflight) {
        client->cancel_semantic = cancel;
        return true;
    }
    /* Each phase can fill the output buffer. Drain earlier frames before taking
     * a final, rather than treating a fast worker as a slow-client overflow. */
    if (!cancel && client->output_length != client->sent)
        return true;
    size_t length = 0;
    size_t capacity = IPC_RESPONSE_BYTES - client->output_length;
    if (capacity <= SEMANTIC_STATUS_BYTES)
        return false;
    char *output = client->output + client->output_length;
    tl_status status = semantic_take(client->semantic, client->slot, client->semantic_token, cancel,
                                     output, capacity - SEMANTIC_STATUS_BYTES, &length);
    if (status == TL_OK && length != 0)
        status = append_semantic_status(daemon, output, capacity, &length);
    if (status != TL_OK)
        return false;
    if (length != 0) {
        client->output_length += length;
        client->semantic_pending = false;
        client->cancel_semantic = false;
    }
    return true;
}
/* IPC thread, under the daemon lock. */
static void semantic_completion(tl_daemon *daemon, struct client *client, bool cancel) {
    if (!take_final(daemon, client, cancel))
        close_client(daemon, client);
}
/* Under the daemon lock: a new query makes every queued query obsolete and
 * cancels a running one. Obsolete queries still answer, as cancelled. */
static void supersede_queries(tl_daemon *daemon, struct client *client) {
    for (size_t i = 0; i < client->queue_count; i++) {
        struct pending *entry = &client->queue[(client->queue_head + i) % DAEMON_ACTIVE_REQUESTS];
        if (entry->decoded != TL_OK || entry->request.operation != IPC_QUERY)
            continue;
        entry->superseded = true;
        if (entry->state == PENDING_RUNNING)
            atomic_store(&daemon->cancel, true);
    }
}
/* Under the daemon lock: decode one frame and queue it for the search thread. */
static void enqueue(tl_daemon *daemon, struct client *client, const char *line, size_t length) {
    size_t index = (client->queue_head + client->queue_count) % DAEMON_ACTIVE_REQUESTS;
    struct pending *entry = &client->queue[index];
    entry->decoded = ipc_decode(line, length, &entry->request);
    entry->repeated = duplicate(client, entry->request.request_id);
    entry->superseded = false;
    entry->state = PENDING_QUEUED;
    bool query = entry->decoded == TL_OK && entry->request.operation == IPC_QUERY;
    /* A repeated active id remains a protocol error, not a cancellation. */
    if (query && !entry->repeated) {
        if (client->semantic_pending)
            semantic_completion(daemon, client, true);
        if (client->fd < 0)
            return;
        supersede_queries(daemon, client);
    }
    client->queue_count++;
    if (entry->decoded != TL_OK || entry->repeated)
        client->closing = true;
    int code = pthread_cond_signal(&daemon->wake);
    (void)code;
}
static void requests(tl_daemon *daemon, struct client *client) {
    char *newline = memchr(client->input, '\n', client->input_length);
    while (newline != NULL && !client->closing && client->fd >= 0) {
        if (client->active_count + client->queue_count >= DAEMON_ACTIVE_REQUESTS) {
            client->closing = true;
            break;
        }
        size_t length = (size_t)(newline - client->input);
        enqueue(daemon, client, client->input, length);
        if (client->fd < 0)
            return;
        size_t rest = client->input_length - length - 1;
        memmove(client->input, newline + 1, rest);
        client->input_length = rest;
        newline = memchr(client->input, '\n', client->input_length);
    }
}
/* A peer that closed its socket (not merely its write half) can never read a
 * reply, so its queued and running work is dropped rather than finished. A
 * zero-byte send distinguishes the two: only a closed peer answers EPIPE. */
static bool peer_gone(int fd) {
    ssize_t count = send(fd, "", 0, MSG_NOSIGNAL);
    return count < 0 && errno == EPIPE;
}
static void receive(tl_daemon *daemon, struct client *client) {
    if (client->input_length == sizeof(client->input)) {
        close_client(daemon, client);
        return;
    }
    ssize_t count = recv(client->fd, client->input + client->input_length,
                         sizeof(client->input) - client->input_length, 0);
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    bool pending = client->output_length > client->sent || client->semantic_pending ||
                   client->queue_count != 0;
    if (count == 0 && pending && !peer_gone(client->fd)) {
        client->closing = true;
        return;
    }
    if (count <= 0) {
        close_client(daemon, client);
        return;
    }
    client->input_length += (size_t)count;
    requests(daemon, client);
    if (client->input_length == sizeof(client->input))
        close_client(daemon, client);
}
/* Under the daemon lock, except for the nonblocking send itself. The search
 * thread only appends, so a shorter snapshot of the queue is still valid. */
static void transmit(tl_daemon *daemon, struct client *client) {
    size_t sent = client->sent, pending = client->output_length;
    unlock_daemon(daemon);
    ssize_t count = send(client->fd, client->output + sent, pending - sent, MSG_NOSIGNAL);
    lock_daemon(daemon);
    if (client->fd < 0)
        return;
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (count <= 0) {
        close_client(daemon, client);
        return;
    }
    client->sent += (size_t)count;
    if (client->sent == client->output_length) {
        client->sent = 0;
        client->output_length = 0;
        client->active_count = 0;
        if (client->semantic_pending)
            memcpy(client->active[client->active_count++], client->pending_id,
                   strlen(client->pending_id) + 1);
        client->deadline = milliseconds() + IPC_DEADLINE_MS;
        if (client->closing && !client->semantic_pending && client->queue_count == 0)
            close_client(daemon, client);
    }
}
static tl_status accept_clients(tl_daemon *daemon) {
    for (size_t count = 0; count < DAEMON_MAX_CLIENTS; count++) {
        int fd = -1;
        tl_status status = ipc_accept(daemon->listener, &fd);
        if (status == TL_STATE)
            return TL_OK;
        if (status != TL_OK)
            return status;
        size_t slot = 0;
        lock_daemon(daemon);
        while (slot < DAEMON_MAX_CLIENTS && daemon->clients[slot].fd >= 0)
            slot++;
        if (slot == DAEMON_MAX_CLIENTS)
            close(fd);
        else {
            daemon->clients[slot].fd = fd;
            daemon->clients[slot].deadline = milliseconds() + IPC_DEADLINE_MS;
        }
        unlock_daemon(daemon);
    }
    return TL_OK;
}
/* Under the daemon lock: per-client housekeeping before each poll. */
static void prepare_poll(tl_daemon *daemon, struct pollfd *fds) {
    uint64_t now = milliseconds();
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++) {
        struct client *client = &daemon->clients[i];
        if (client->fd >= 0 && client->abort)
            close_client(daemon, client);
        if (client->fd >= 0)
            semantic_completion(daemon, client, false);
        if (client->fd >= 0 && now >= client->deadline)
            close_client(daemon, client);
        short events = client->closing ? 0 : POLLIN;
        if (client->output_length > client->sent)
            events |= POLLOUT;
        fds[i + 2] = (struct pollfd){client->fd, events, 0};
    }
}
/* Under the daemon lock: service one client's socket events. */
static void service_client(tl_daemon *daemon, struct client *client, short events) {
    if (client->fd < 0)
        return;
    /* POLLHUP means both directions are shut: the peer closed, so nothing it
     * still sent can be answered. Half-closed peers never raise it. */
    if ((events & (POLLERR | POLLNVAL | POLLHUP)) != 0) {
        close_client(daemon, client);
        return;
    }
    if ((events & POLLIN) != 0)
        receive(daemon, client);
    if (client->fd >= 0 && (events & POLLOUT) != 0 && client->output_length > client->sent)
        transmit(daemon, client);
}
tl_status daemon_run(tl_daemon *daemon) {
    if (daemon == NULL)
        return TL_INVALID;
    enum { SEMANTIC_SLOT = DAEMON_MAX_CLIENTS + 2, DONE_SLOT = DAEMON_MAX_CLIENTS + 3 };
    for (;;) {
        struct pollfd fds[DAEMON_MAX_CLIENTS + 4] = {
            {ipc_listener_descriptor(daemon->listener), POLLIN, 0}, {daemon->signal_fd, POLLIN, 0}};
        fds[SEMANTIC_SLOT] = (struct pollfd){semantic_descriptor(daemon->semantic), POLLIN, 0};
        fds[DONE_SLOT] = (struct pollfd){daemon->done_fd, POLLIN, 0};
        lock_daemon(daemon);
        prepare_poll(daemon, fds);
        unlock_daemon(daemon);
        int code = poll(fds, DAEMON_MAX_CLIENTS + 4, daemon->semantic == NULL ? 50 : 10);
        if (code < 0 && errno == EINTR)
            continue;
        if (code < 0)
            return TL_IO;
        if ((fds[SEMANTIC_SLOT].revents & POLLIN) != 0)
            semantic_drain(daemon->semantic);
        if ((fds[DONE_SLOT].revents & POLLIN) != 0) {
            uint64_t completions = 0;
            ssize_t count = read(daemon->done_fd, &completions, sizeof(completions));
            (void)count;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            struct signalfd_siginfo event;
            ssize_t count = read(daemon->signal_fd, &event, sizeof(event));
            return count == (ssize_t)sizeof(event) ? TL_OK : TL_IO;
        }
        if ((fds[0].revents & POLLIN) != 0) {
            tl_status status = accept_clients(daemon);
            if (status != TL_OK)
                return status;
        }
        lock_daemon(daemon);
        for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++)
            service_client(daemon, &daemon->clients[i], fds[i + 2].revents);
        unlock_daemon(daemon);
    }
}
