/* GIO nonblocking socket framing, validation, peer checks and cancellation. */
#include "torchlight/async.h"
#include <gio/gio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define ASYNC_JSON_TOKENS 16384
#define ASYNC_READ_BYTES 4096
struct tl_ipc_exchange {
    unsigned refs;
    bool disposed, complete;
    guint timeout;
    GCancellable *cancel;
    GSocketClient *client;
    GSocketConnection *connection;
    tl_ipc_request request;
    tl_ipc_callback callback;
    void *context;
    char outgoing[IPC_REQUEST_BYTES];
    size_t outgoing_length, length;
    char *response;
    tl_json_token *tokens;
};
static void exchange_unref(tl_ipc_exchange *exchange) {
    if (--exchange->refs != 0)
        return;
    if (exchange->connection != NULL)
        g_object_unref(exchange->connection);
    exchange->connection = NULL;
    if (exchange->client != NULL)
        g_object_unref(exchange->client);
    exchange->client = NULL;
    if (exchange->cancel != NULL)
        g_object_unref(exchange->cancel);
    exchange->cancel = NULL;
    g_free(exchange->response);
    g_free(exchange->tokens);
    g_free(exchange);
}
static void deliver(tl_ipc_exchange *exchange, tl_status status, bool terminal) {
    if (exchange->complete)
        return;
    if (terminal) {
        exchange->complete = true;
        if (exchange->timeout != 0) {
            g_source_remove(exchange->timeout);
            exchange->timeout = 0;
        }
    }
    if (!exchange->disposed)
        exchange->callback(exchange->context, status, exchange->response, exchange->length,
                           terminal);
}
static gboolean deadline(gpointer context) {
    tl_ipc_exchange *exchange = context;
    exchange->timeout = 0;
    g_cancellable_cancel(exchange->cancel);
    deliver(exchange, TL_IO, true);
    return G_SOURCE_REMOVE;
}
static tl_status validate(tl_ipc_exchange *exchange, bool *terminal) {
    tl_json json;
    tl_status status = json_parse(exchange->response, exchange->length, exchange->tokens,
                                  ASYNC_JSON_TOKENS, &json);
    if (status != TL_OK)
        return status;
    char id[IPC_REQUEST_ID_BYTES + 1], phase[16];
    uint64_t version = 0;
    if (json_uint(&json, json_member(&json, 0, "version"), &version) != TL_OK ||
        version != IPC_VERSION ||
        json_string(&json, json_member(&json, 0, "request_id"), id, sizeof(id)) != TL_OK ||
        strcmp(id, exchange->request.request_id) != 0 ||
        json_string(&json, json_member(&json, 0, "phase"), phase, sizeof(phase)) != TL_OK)
        return TL_INVALID;
    *terminal = strcmp(phase, "final") == 0;
    return *terminal || strcmp(phase, "lexical") == 0 ? TL_OK : TL_INVALID;
}
static void read_next(tl_ipc_exchange *exchange);
static void received(GObject *source, GAsyncResult *result, gpointer context) {
    tl_ipc_exchange *exchange = context;
    GError *error = NULL;
    GBytes *bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
    gsize count = 0;
    const char *data = bytes == NULL ? NULL : g_bytes_get_data(bytes, &count);
    tl_status status = error != NULL || count == 0 ? TL_IO : TL_OK;
    for (size_t i = 0; status == TL_OK && i < count && !exchange->complete; i++) {
        if (exchange->length + 1 >= IPC_RESPONSE_BYTES) {
            status = TL_LIMIT;
            break;
        }
        exchange->response[exchange->length++] = data[i];
        if (data[i] != '\n')
            continue;
        exchange->response[exchange->length] = 0;
        bool terminal = false;
        status = validate(exchange, &terminal);
        if (status == TL_OK)
            deliver(exchange, status, terminal);
        exchange->length = 0;
    }
    if (bytes != NULL)
        g_bytes_unref(bytes);
    g_clear_error(&error);
    if (status != TL_OK)
        deliver(exchange, status, true);
    if (!exchange->disposed && !exchange->complete)
        read_next(exchange);
    exchange_unref(exchange);
}
static void read_next(tl_ipc_exchange *exchange) {
    exchange->refs++;
    g_input_stream_read_bytes_async(g_io_stream_get_input_stream(G_IO_STREAM(exchange->connection)),
                                    ASYNC_READ_BYTES, G_PRIORITY_DEFAULT, exchange->cancel,
                                    received, exchange);
}
static void written(GObject *source, GAsyncResult *result, gpointer context) {
    tl_ipc_exchange *exchange = context;
    GError *error = NULL;
    gsize count = 0;
    bool ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, &count, &error);
    g_clear_error(&error);
    if (!ok || count != exchange->outgoing_length)
        deliver(exchange, TL_IO, true);
    else if (!exchange->disposed && !exchange->complete)
        read_next(exchange);
    exchange_unref(exchange);
}
static void connected(GObject *source, GAsyncResult *result, gpointer context) {
    tl_ipc_exchange *exchange = context;
    GError *error = NULL;
    exchange->connection = g_socket_client_connect_finish(G_SOCKET_CLIENT(source), result, &error);
    g_clear_error(&error);
    struct ucred peer;
    socklen_t length = sizeof(peer);
    bool ok = exchange->connection != NULL;
    if (ok) {
        int fd = g_socket_get_fd(g_socket_connection_get_socket(exchange->connection));
        ok = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) == 0 && peer.uid == getuid();
    }
    if (!ok)
        deliver(exchange, TL_IO, true);
    else if (!exchange->disposed && !exchange->complete) {
        exchange->refs++;
        g_output_stream_write_all_async(
            g_io_stream_get_output_stream(G_IO_STREAM(exchange->connection)), exchange->outgoing,
            exchange->outgoing_length, G_PRIORITY_DEFAULT, exchange->cancel, written, exchange);
    }
    exchange_unref(exchange);
}
tl_status ipc_exchange_create(const char *socket_path, const tl_ipc_request *request,
                              tl_ipc_callback callback, void *context, tl_ipc_exchange **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (socket_path == NULL || socket_path[0] != '/' || request == NULL || callback == NULL)
        return TL_INVALID;
    tl_ipc_exchange *exchange = g_try_new0(tl_ipc_exchange, 1);
    if (exchange == NULL)
        return TL_NOMEM;
    exchange->refs = 1;
    exchange->request = *request;
    exchange->callback = callback;
    exchange->context = context;
    exchange->response = g_try_malloc(IPC_RESPONSE_BYTES);
    exchange->tokens = g_try_new(tl_json_token, ASYNC_JSON_TOKENS);
    if (exchange->response == NULL || exchange->tokens == NULL) {
        exchange_unref(exchange);
        return TL_NOMEM;
    }
    tl_status status = ipc_encode(request, exchange->outgoing, sizeof(exchange->outgoing),
                                  &exchange->outgoing_length);
    if (status != TL_OK) {
        exchange_unref(exchange);
        return status;
    }
    exchange->cancel = g_cancellable_new();
    exchange->client = g_socket_client_new();
    g_socket_client_set_timeout(exchange->client, IPC_DEADLINE_MS / 1000);
    exchange->timeout = g_timeout_add(IPC_DEADLINE_MS, deadline, exchange);
    GSocketAddress *address = g_unix_socket_address_new(socket_path);
    exchange->refs++;
    g_socket_client_connect_async(exchange->client, G_SOCKET_CONNECTABLE(address), exchange->cancel,
                                  connected, exchange);
    g_object_unref(address);
    *out = exchange;
    return TL_OK;
}
void ipc_exchange_destroy(tl_ipc_exchange *exchange) {
    if (exchange == NULL || exchange->disposed)
        return;
    exchange->disposed = true;
    if (exchange->timeout != 0) {
        g_source_remove(exchange->timeout);
        exchange->timeout = 0;
    }
    g_cancellable_cancel(exchange->cancel);
    exchange_unref(exchange);
}
