/* Fake socket phases validate asynchronous framing, deadlines and stale IDs. */
#include "test.h"
#include "torchlight/async.h"
#include <gio/gio.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct fake_service {
    tl_ipc_listener *listener;
    const char *reply;
};
struct callback_state {
    size_t phases, terminals;
    tl_status status;
    bool done;
};
static void *fake_reply(void *context) {
    struct fake_service *service = context;
    int client = -1;
    struct timespec pause = {.tv_nsec = 1000000};
    tl_status status = TL_STATE;
    while (status == TL_STATE) {
        status = ipc_accept(service->listener, &client);
        if (status == TL_STATE)
            nanosleep(&pause, NULL);
    }
    CHECK(status == TL_OK);
    char request[IPC_REQUEST_BYTES];
    ssize_t received = -1;
    while (received < 0) {
        received = recv(client, request, sizeof(request), 0);
        if (received < 0)
            nanosleep(&pause, NULL);
    }
    CHECK(received > 0);
    /* Fragment every byte: response framing must not assume whole recv lines. */
    size_t length = strlen(service->reply);
    for (size_t i = 0; i < length; i++)
        CHECK(send(client, service->reply + i, 1, MSG_NOSIGNAL) == 1);
    close(client);
    return NULL;
}
static void callback(void *context, tl_status status, const char *response, size_t length,
                     bool terminal) {
    struct callback_state *state = context;
    state->status = status;
    state->phases++;
    if (status == TL_OK)
        CHECK(response != NULL && length != 0 && response[length - 1] == '\n');
    if (terminal) {
        state->terminals++;
        state->done = true;
    }
}
static void run_reply(const char *path, tl_ipc_listener *listener, const char *reply,
                      tl_status expected, size_t phases) {
    struct fake_service service = {listener, reply};
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, fake_reply, &service) == 0);
    tl_ipc_request request = {.operation = IPC_QUERY, .request_id = "request", .limit = 10};
    tl_ipc_exchange *exchange = NULL;
    struct callback_state state = {0};
    CHECK(ipc_exchange_create(path, &request, callback, &state, &exchange) == TL_OK);
    while (!state.done)
        g_main_context_iteration(NULL, true);
    CHECK(state.status == expected && state.phases == phases && state.terminals == 1);
    ipc_exchange_destroy(exchange);
    CHECK(pthread_join(worker, NULL) == 0);
}
void test_async(void) {
    char directory[] = "/tmp/torchlight-async-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256], lock[256];
    CHECK(snprintf(path, sizeof(path), "%s/socket", directory) > 0);
    CHECK(snprintf(lock, sizeof(lock), "%s.lock", path) > 0);
    tl_ipc_listener *listener = NULL;
    CHECK(ipc_listener_create(path, &listener) == TL_OK);
    run_reply(path, listener,
              "{\"version\":1,\"request_id\":\"request\",\"phase\":\"lexical\"}\n"
              "{\"version\":1,\"request_id\":\"request\",\"phase\":\"final\"}\n",
              TL_OK, 2);
    run_reply(path, listener, "{\"version\":1,\"request_id\":\"obsolete\",\"phase\":\"final\"}\n",
              TL_INVALID, 1);
    run_reply(path, listener, "{\"version\":1", TL_IO, 1);
    ipc_listener_destroy(listener);
    CHECK(unlink(lock) == 0 && rmdir(directory) == 0);
}
