/* Resident two-phase query loop; sockets never retain a lexical workspace lease. */
#include "torchlight/daemon.h"
#include "torchlight/desktop.h"
#include "torchlight/semantic.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct client {
    tl_semantic *semantic;
    size_t slot;
    uint64_t semantic_token;
    bool semantic_pending;
    char pending_id[IPC_REQUEST_ID_BYTES + 1];
    int fd;
    char input[IPC_REQUEST_BYTES];
    size_t input_length;
    char *output;
    size_t output_length, sent;
    char active[DAEMON_ACTIVE_REQUESTS][IPC_REQUEST_ID_BYTES + 1];
    size_t active_count;
    bool closing;
    uint64_t deadline;
};
struct tl_daemon {
    tl_catalog *catalog;
    tl_desktop *desktop;
    tl_writer *writer;
    tl_semantic *semantic;
    tl_ipc_listener *listener;
    int signal_fd, database_lock;
    sigset_t old_mask;
    bool masked;
    char session[33];
    uint64_t search_sequence;
    uint64_t engine_ns;
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
}
static void close_client(struct client *client) {
    discard_semantic(client);
    if (client->fd >= 0)
        close(client->fd);
    client->fd = -1;
    client->input_length = 0;
    client->output_length = 0;
    client->sent = 0;
    client->active_count = 0;
    client->closing = false;
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
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++)
        daemon->clients[i].fd = -1;
    tl_status status = ipc_database_lock(config_database(options->config), &daemon->database_lock);
    if (status == TL_OK)
        status = signals(daemon);
    if (status == TL_OK)
        status = session_id(daemon->session);
    if (status == TL_OK)
        status = catalog_create(2, &daemon->catalog);
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS && status == TL_OK; i++) {
        daemon->clients[i].output = malloc(IPC_RESPONSE_BYTES);
        if (daemon->clients[i].output == NULL)
            status = TL_NOMEM;
    }
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
    if (status == TL_OK)
        status = writer_create(&writer_options, &daemon->writer);
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
    if (status == TL_OK)
        status = ipc_listener_create(options->socket_path, &daemon->listener);
    if (status != TL_OK) {
        tl_status cleanup = daemon_destroy(daemon);
        if (cleanup != TL_OK)
            return cleanup;
        return status;
    }
    *out = daemon;
    return TL_OK;
}
tl_status daemon_destroy(tl_daemon *daemon) {
    if (daemon == NULL)
        return TL_OK;
    for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++) {
        close_client(&daemon->clients[i]);
        free(daemon->clients[i].output);
        daemon->clients[i].output = NULL;
    }
    ipc_listener_destroy(daemon->listener);
    daemon->listener = NULL;
    semantic_destroy(daemon->semantic);
    desktop_destroy(daemon->desktop);
    writer_destroy(daemon->writer);
    daemon->writer = NULL;
    tl_status status = catalog_destroy(daemon->catalog);
    if (status != TL_OK)
        return status;
    daemon->catalog = NULL;
    if (daemon->signal_fd >= 0)
        close(daemon->signal_fd);
    if (daemon->masked) {
        int code = pthread_sigmask(SIG_SETMASK, &daemon->old_mask, NULL);
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
                            const tl_semantic_stats *semantic) {
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
                            const tl_semantic_stats *semantic) {
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
    indexing_status(b, stats, semantic);
    json_raw(b, ",\"results\":[");
}
static tl_status merge_applications(tl_daemon *daemon, const tl_ipc_request *request,
                                    size_t *count) {
    size_t app_count = 0;
    tl_status status = desktop_query(daemon->desktop, request->query, daemon->applications,
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
    tl_status status = writer_history(daemon->writer, &event, NULL);
    return status == TL_STATE ? TL_OK : status;
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
        tl_status history = writer_history(daemon->writer, request, search_id);
        if (history != TL_OK && history != TL_LIMIT && history != TL_STATE)
            return history;
        if (superseded)
            return TL_STATE;
    }
    if ((request->operation == IPC_RESOLVE || request->operation == IPC_OPEN) &&
        request->file_id >= DESKTOP_ID_BASE)
        return resolve_application(daemon, request, count);
    if (request->operation == IPC_QUERY || request->operation == IPC_RESOLVE ||
        request->operation == IPC_OPEN) {
        tl_status status = catalog_acquire(daemon->catalog, reader);
        if (status != TL_OK)
            return status;
        *catalog_gen = catalog_reader_gen(*reader);
        if (request->operation == IPC_QUERY) {
            uint64_t start = nanoseconds();
            status = catalog_query(*reader, request->query, daemon->results, request->limit, count);
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
            status = writer_history(daemon->writer, request, NULL);
            if (status == TL_STATE)
                status = TL_OK;
        }
        return status;
    }
    if (request->operation == IPC_RECONCILE) {
        desktop_refresh(daemon->desktop);
        return writer_reconcile(daemon->writer);
    }
    if (request->operation == IPC_HISTORY_CLEAR)
        return writer_history(daemon->writer, request, NULL);
    return TL_OK;
}
static bool duplicate(const struct client *client, const char *id) {
    for (size_t i = 0; i < client->active_count; i++)
        if (strcmp(client->active[i], id) == 0)
            return true;
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
    json_raw(buffer, "}");
}
static tl_status encode_completion(tl_daemon *daemon, struct client *client,
                                   const struct completion *completion, size_t *length) {
    char *output = client->output + client->output_length;
    size_t capacity = IPC_RESPONSE_BYTES - client->output_length;
    const char *status = completion->status == TL_OK ? "ok"
                         : completion->superseded    ? "cancelled"
                                                     : "error";
    tl_json_buffer b;
    json_buffer_init(&b, output, capacity);
    response_prefix(&b, completion->request, completion->catalog_gen, completion->search_id, status,
                    completion->reason, &completion->stats, completion->lexical_phase,
                    completion->emb_gen, daemon->semantic == NULL ? NULL : &completion->semantic);
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
        discard_semantic(client);
        json_buffer_init(&b, output, capacity);
        response_prefix(&b, completion->request, completion->catalog_gen, completion->search_id,
                        "error", "response_limit", &completion->stats, false, completion->emb_gen,
                        daemon->semantic == NULL ? NULL : &completion->semantic);
        json_raw(&b, "]}\n");
    }
    *length = b.status == TL_OK ? b.length : 0;
    return b.status;
}
static void queue_semantic(tl_daemon *daemon, struct client *client,
                           struct completion *completion) {
    const tl_ipc_request *request = completion->request;
    bool superseded = completion->superseded;
    if (daemon->semantic != NULL && request->operation == IPC_QUERY &&
        completion->status == TL_OK && !superseded && client->semantic_token < UINT64_MAX) {
        tl_status queued = semantic_submit(daemon->semantic, client->slot, ++client->semantic_token,
                                           request, completion->search_id, completion->catalog_gen,
                                           desktop_gen(daemon->desktop), daemon->results,
                                           completion->count, &completion->emb_gen);
        if (queued == TL_OK) {
            completion->lexical_phase = true;
            completion->reason = "semantic_pending";
            client->semantic_pending = true;
            memcpy(client->pending_id, request->request_id, strlen(request->request_id) + 1);
        } else {
            completion->reason =
                queued == TL_LIMIT ? "semantic_queue_full" : "semantic_unavailable";
        }
    }
}
static void respond(tl_daemon *daemon, struct client *client, const tl_ipc_request *request,
                    tl_status decoded, bool superseded) {
    struct completion completion = {.request = request, .superseded = superseded};
    tl_catalog_stats catalog = {0};
    completion.status = writer_stats(daemon->writer, &completion.stats);
    if (completion.status == TL_OK && daemon->semantic != NULL) {
        completion.status = semantic_stats(daemon->semantic, &completion.semantic);
        if (request->operation == IPC_STATUS)
            completion.emb_gen = completion.semantic.emb_gen;
    }
    if (completion.status == TL_OK)
        completion.status = catalog_stats(daemon->catalog, &catalog);
    if (decoded != TL_OK)
        completion.status = decoded;
    bool repeated = duplicate(client, request->request_id);
    if (repeated)
        completion.status = TL_INVALID;
    completion.catalog_gen = catalog.catalog_gen;
    tl_catalog_reader *reader = NULL;
    daemon->engine_ns = 0;
    desktop_acquire(daemon->desktop);
    tl_ipc_request pooled = *request;
    if (daemon->semantic != NULL && request->operation == IPC_QUERY)
        pooled.limit = LEXICAL_MAX_RESULTS;
    if (completion.status == TL_OK)
        completion.status = execute(daemon, &pooled, completion.search_id, &completion.count,
                                    &completion.catalog_gen, &reader, superseded);
    completion.reader = reader;
    completion.reason = response_reason(request, completion.status, decoded, repeated, superseded);
    queue_semantic(daemon, client, &completion);
    size_t length = 0;
    tl_status encoded = encode_completion(daemon, client, &completion, &length);
    catalog_release(reader);
    desktop_release(daemon->desktop);
    if (encoded != TL_OK) {
        close_client(client);
        return;
    }
    client->output_length += length;
    if (client->active_count < DAEMON_ACTIVE_REQUESTS)
        memcpy(client->active[client->active_count++], request->request_id,
               strlen(request->request_id) + 1);
    if (decoded != TL_OK || repeated)
        client->closing = true;
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
    indexing_status(&buffer, &writer, &semantic);
    json_raw(&buffer, "}\n");
    *length = buffer.status == TL_OK ? prefix + buffer.length : 0;
    return buffer.status;
}
static void semantic_completion(tl_daemon *daemon, struct client *client, bool cancel) {
    if (!client->semantic_pending)
        return;
    /* Each phase can fill the output buffer. Drain earlier frames before taking
     * a final, rather than treating a fast worker as a slow-client overflow. */
    if (!cancel && client->output_length != client->sent)
        return;
    size_t length = 0;
    size_t capacity = IPC_RESPONSE_BYTES - client->output_length;
    if (capacity <= SEMANTIC_STATUS_BYTES) {
        close_client(client);
        return;
    }
    char *output = client->output + client->output_length;
    tl_status status = semantic_take(client->semantic, client->slot, client->semantic_token, cancel,
                                     output, capacity - SEMANTIC_STATUS_BYTES, &length);
    if (status == TL_OK && length != 0)
        status = append_semantic_status(daemon, output, capacity, &length);
    if (status != TL_OK) {
        close_client(client);
        return;
    }
    if (length != 0) {
        client->output_length += length;
        client->semantic_pending = false;
    }
}
static bool newer_query(const char *data, size_t length) {
    const char *newline = memchr(data, '\n', length);
    while (newline != NULL) {
        tl_ipc_request request;
        if (ipc_decode(data, (size_t)(newline - data), &request) == TL_OK &&
            request.operation == IPC_QUERY)
            return true;
        size_t used = (size_t)(newline - data) + 1;
        data += used;
        length -= used;
        newline = memchr(data, '\n', length);
    }
    return false;
}
static void requests(tl_daemon *daemon, struct client *client) {
    char *newline = memchr(client->input, '\n', client->input_length);
    while (newline != NULL && !client->closing && client->fd >= 0) {
        if (client->active_count == DAEMON_ACTIVE_REQUESTS) {
            client->closing = true;
            break;
        }
        size_t length = (size_t)(newline - client->input);
        tl_ipc_request request;
        tl_status decoded = ipc_decode(client->input, length, &request);
        size_t rest = client->input_length - length - 1;
        bool superseded =
            decoded == TL_OK && request.operation == IPC_QUERY && newer_query(newline + 1, rest);
        /* A repeated active id remains a protocol error, not a cancellation. */
        if (decoded == TL_OK && request.operation == IPC_QUERY && client->semantic_pending &&
            !duplicate(client, request.request_id))
            semantic_completion(daemon, client, true);
        if (client->fd < 0)
            return;
        respond(daemon, client, &request, decoded, superseded);
        if (client->fd < 0)
            return;
        memmove(client->input, newline + 1, rest);
        client->input_length = rest;
        newline = memchr(client->input, '\n', client->input_length);
    }
}
static void receive(tl_daemon *daemon, struct client *client) {
    if (client->input_length == sizeof(client->input)) {
        close_client(client);
        return;
    }
    ssize_t count = recv(client->fd, client->input + client->input_length,
                         sizeof(client->input) - client->input_length, 0);
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (count == 0 && (client->output_length > client->sent || client->semantic_pending)) {
        client->closing = true;
        return;
    }
    if (count <= 0) {
        close_client(client);
        return;
    }
    client->input_length += (size_t)count;
    requests(daemon, client);
    if (client->input_length == sizeof(client->input))
        close_client(client);
}
static void transmit(struct client *client) {
    ssize_t count = send(client->fd, client->output + client->sent,
                         client->output_length - client->sent, MSG_NOSIGNAL);
    if (count < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (count <= 0) {
        close_client(client);
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
        if (client->closing && !client->semantic_pending)
            close_client(client);
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
        while (slot < DAEMON_MAX_CLIENTS && daemon->clients[slot].fd >= 0)
            slot++;
        if (slot == DAEMON_MAX_CLIENTS)
            close(fd);
        else {
            daemon->clients[slot].fd = fd;
            daemon->clients[slot].deadline = milliseconds() + IPC_DEADLINE_MS;
        }
    }
    return TL_OK;
}
tl_status daemon_run(tl_daemon *daemon) {
    if (daemon == NULL)
        return TL_INVALID;
    for (;;) {
        struct pollfd fds[DAEMON_MAX_CLIENTS + 3] = {
            {ipc_listener_descriptor(daemon->listener), POLLIN, 0}, {daemon->signal_fd, POLLIN, 0}};
        fds[DAEMON_MAX_CLIENTS + 2] =
            (struct pollfd){semantic_descriptor(daemon->semantic), POLLIN, 0};
        uint64_t now = milliseconds();
        for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++) {
            struct client *client = &daemon->clients[i];
            if (client->fd >= 0)
                semantic_completion(daemon, client, false);
            if (client->fd >= 0 && now >= client->deadline)
                close_client(client);
            short events = client->closing ? 0 : POLLIN;
            if (client->output_length > client->sent)
                events |= POLLOUT;
            fds[i + 2] = (struct pollfd){client->fd, events, 0};
        }
        int code = poll(fds, DAEMON_MAX_CLIENTS + 3, daemon->semantic == NULL ? 50 : 10);
        if (code < 0 && errno == EINTR)
            continue;
        if (code < 0)
            return TL_IO;
        if ((fds[DAEMON_MAX_CLIENTS + 2].revents & POLLIN) != 0)
            semantic_drain(daemon->semantic);
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
        for (size_t i = 0; i < DAEMON_MAX_CLIENTS; i++) {
            struct client *client = &daemon->clients[i];
            short events = fds[i + 2].revents;
            if (client->fd < 0)
                continue;
            if ((events & (POLLERR | POLLNVAL)) != 0) {
                close_client(client);
                continue;
            }
            if ((events & POLLIN) != 0)
                receive(daemon, client);
            if (client->fd >= 0 && (events & POLLOUT) != 0 && client->output_length > client->sent)
                transmit(client);
            if (client->fd >= 0 && (events & POLLHUP) != 0 && (events & POLLIN) == 0 &&
                client->output_length == client->sent)
                close_client(client);
        }
    }
}
