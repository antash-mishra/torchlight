/* Protocol field validation and Unix socket singleton/ownership checks. */
#include "test.h"
#include "torchlight/ipc.h"
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
static void protocol(void) {
    tl_ipc_request request;
    const char *valid = "{\"version\":1,\"op\":\"query\",\"request_id\":\"r\",\"query\":"
                        "\"\\u00e9\\n\",\"limit\":1000}";
    CHECK(ipc_decode(valid, strlen(valid), &request) == TL_OK && request.limit == 1000 &&
          strcmp(request.query, "\xc3\xa9\n") == 0);
    char encoded[IPC_REQUEST_BYTES];
    size_t length = 0;
    CHECK(ipc_encode(&request, encoded, sizeof(encoded), &length) == TL_OK);
    tl_ipc_request copy;
    CHECK(ipc_decode(encoded, length - 1, &copy) == TL_OK &&
          strcmp(copy.query, request.query) == 0);
    const char *invalid[] = {
        "{}",
        "[]",
        "{\"version\":\"1\",\"op\":\"status\",\"request_id\":\"r\"}",
        "{\"version\":2,\"op\":\"status\",\"request_id\":\"r\"}",
        "{\"version\":1,\"op\":\"status\",\"request_id\":\"\"}",
        "{\"version\":1,\"op\":\"query\",\"request_id\":\"r\"}",
        "{\"version\":1,\"op\":\"query\",\"request_id\":\"r\",\"query\":null}",
        "{\"version\":1,\"op\":\"query\",\"request_id\":\"r\",\"query\":\"x\",\"limit\":0}",
        "{\"version\":1,\"op\":\"query\",\"request_id\":\"r\",\"query\":\"x\",\"limit\":1001}",
        "{\"version\":1,\"op\":\"status\",\"request_id\":\"r\",\"extra\":1}",
        "{\"version\":1,\"op\":\"resolve\",\"request_id\":\"r\",\"file_id\":1}",
        "{\"version\":1,\"op\":\"resolve\",\"request_id\":\"r\",\"file_id\":"
        "\"9223372036854775808\"}",
        "{\"version\":1,\"op\":\"open\",\"request_id\":\"r\",\"file_id\":\"1\"}"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        CHECK(ipc_decode(invalid[i], strlen(invalid[i]), &request) == TL_INVALID);
    CHECK(ipc_decode(valid, IPC_REQUEST_BYTES, &request) == TL_LIMIT);
    CHECK(ipc_decode(NULL, 0, &request) == TL_INVALID);
    memset(&request, 0xff, sizeof(request));
    request.operation = IPC_QUERY;
    CHECK(ipc_encode(&request, encoded, sizeof(encoded), &length) == TL_INVALID && length == 0);
}
static void listener_lifecycle(void) {
    char directory[] = "/tmp/torchlight-ipc-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[256], lock[256];
    CHECK(snprintf(path, sizeof(path), "%s/socket", directory) > 0);
    CHECK(snprintf(lock, sizeof(lock), "%s/socket.lock", directory) > 0);
    tl_ipc_listener *first = NULL, *second = NULL;
    CHECK(ipc_listener_create(path, &first) == TL_OK);
    struct stat info;
    CHECK(stat(path, &info) == 0 && (info.st_mode & 0777) == 0600);
    CHECK(ipc_listener_create(path, &second) == TL_STATE && second == NULL);
    int fd = -1;
    CHECK(ipc_accept(first, &fd) == TL_STATE && fd == -1);
    ipc_listener_destroy(first);
    CHECK(lstat(path, &info) != 0);
    CHECK(ipc_listener_create(path, &first) == TL_OK);
    ipc_listener_destroy(first);
    CHECK(unlink(lock) == 0);
    CHECK(symlink("/dev/null", lock) == 0);
    CHECK(ipc_listener_create(path, &first) == TL_IO && first == NULL);
    CHECK(unlink(lock) == 0);
    FILE *stream = fopen(path, "w");
    CHECK(stream != NULL && fclose(stream) == 0);
    CHECK(ipc_listener_create(path, &first) == TL_IO && first == NULL);
    CHECK(unlink(path) == 0 && unlink(lock) == 0);
    int unmanaged = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(unmanaged >= 0);
    struct sockaddr_un socket_address = {.sun_family = AF_UNIX};
    CHECK(strlen(path) < sizeof(socket_address.sun_path));
    memcpy(socket_address.sun_path, path, strlen(path) + 1);
    CHECK(bind(unmanaged, (const struct sockaddr *)&socket_address, sizeof(socket_address)) == 0 &&
          listen(unmanaged, 1) == 0);
    CHECK(ipc_listener_create(path, &first) == TL_STATE && first == NULL);
    CHECK(close(unmanaged) == 0);
    CHECK(ipc_listener_create(path, &first) == TL_OK);
    ipc_listener_destroy(first);
    CHECK(unlink(lock) == 0 && rmdir(directory) == 0);
}
void test_ipc(void) {
    protocol();
    listener_lifecycle();
}
