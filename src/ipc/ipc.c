/* Strict protocol codec, singleton socket lifecycle and deadline-bound clients. */
#include "torchlight/ipc.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
struct tl_ipc_listener {
    int fd, lock;
    char *path;
    bool bound;
};
static const char *const OPERATIONS[] = {"query", "status",    "resolve",
                                         "open",  "reconcile", "history_clear"};
static tl_status string_member(const tl_json *json, const char *key, char *out, size_t capacity,
                               bool required) {
    size_t token = json_member(json, 0, key);
    if (token == SIZE_MAX)
        return required ? TL_INVALID : TL_OK;
    tl_status status = json_string(json, token, out, capacity);
    return status == TL_OK && required && out[0] == 0 ? TL_INVALID : status;
}
static bool allowed_key(tl_ipc_operation op, const char *key) {
    if (strcmp(key, "version") == 0 || strcmp(key, "request_id") == 0 || strcmp(key, "op") == 0)
        return true;
    if (op == IPC_QUERY)
        return strcmp(key, "query") == 0 || strcmp(key, "limit") == 0;
    if (op == IPC_RESOLVE || op == IPC_OPEN) {
        if (strcmp(key, "file_id") == 0)
            return true;
        if (op == IPC_OPEN)
            return strcmp(key, "search_id") == 0 || strcmp(key, "event_id") == 0;
    }
    return false;
}
static tl_status decode_fields(const tl_json *json, tl_ipc_request *request) {
    tl_status status =
        string_member(json, "request_id", request->request_id, sizeof(request->request_id), true);
    if (status == TL_OK && request->operation == IPC_QUERY) {
        if (json_member(json, 0, "query") == SIZE_MAX)
            return TL_INVALID;
        status = string_member(json, "query", request->query, sizeof(request->query), false);
        size_t limit = json_member(json, 0, "limit");
        uint64_t count = 0;
        if (status == TL_OK && limit != SIZE_MAX) {
            if (json->tokens[limit].type != JSON_NUMBER ||
                json_uint(json, limit, &count) != TL_OK || count == 0 ||
                count > LEXICAL_MAX_RESULTS)
                return TL_INVALID;
            request->limit = (size_t)count;
        }
    }
    if (status == TL_OK && (request->operation == IPC_RESOLVE || request->operation == IPC_OPEN)) {
        size_t id = json_member(json, 0, "file_id");
        if (id == SIZE_MAX || json->tokens[id].type != JSON_STRING ||
            json_uint(json, id, &request->file_id) != TL_OK || request->file_id == 0 ||
            request->file_id > INT64_MAX)
            return TL_INVALID;
    }
    if (status == TL_OK && request->operation == IPC_OPEN)
        status =
            string_member(json, "event_id", request->event_id, sizeof(request->event_id), true);
    if (status == TL_OK && request->operation == IPC_OPEN)
        status =
            string_member(json, "search_id", request->search_id, sizeof(request->search_id), false);
    return status;
}
tl_status ipc_decode(const char *line, size_t length, tl_ipc_request *out) {
    if (out == NULL)
        return TL_INVALID;
    *out = (tl_ipc_request){0};
    if (length >= IPC_REQUEST_BYTES)
        return TL_LIMIT;
    tl_json_token tokens[IPC_REQUEST_TOKENS];
    tl_json json;
    tl_status status = json_parse(line, length, tokens, IPC_REQUEST_TOKENS, &json);
    if (status != TL_OK || tokens[0].type != JSON_OBJECT)
        return status == TL_OK ? TL_INVALID : status;
    uint64_t version = 0;
    size_t version_token = json_member(&json, 0, "version");
    if (version_token == SIZE_MAX || tokens[version_token].type != JSON_NUMBER ||
        json_uint(&json, version_token, &version) != TL_OK || version != IPC_VERSION)
        return TL_INVALID;
    char operation[32];
    tl_ipc_request request = {.limit = 10};
    status = string_member(&json, "op", operation, sizeof(operation), true);
    if (status != TL_OK)
        return status;
    size_t op = 0;
    while (op < sizeof(OPERATIONS) / sizeof(OPERATIONS[0]) &&
           strcmp(operation, OPERATIONS[op]) != 0)
        op++;
    if (op == sizeof(OPERATIONS) / sizeof(OPERATIONS[0]))
        return TL_INVALID;
    request.operation = (tl_ipc_operation)op;
    for (size_t i = 1; i < tokens[0].next;) {
        char key[JSON_KEY_BYTES + 1];
        status = json_string(&json, i, key, sizeof(key));
        if (status != TL_OK || !allowed_key(request.operation, key))
            return TL_INVALID;
        i = tokens[i + 1].next;
    }
    status = decode_fields(&json, &request);
    if (status == TL_OK)
        *out = request;
    return status;
}
static void field(tl_json_buffer *b, const char *key, const char *value) {
    json_raw(b, ",");
    json_quote(b, key);
    json_raw(b, ":");
    json_quote(b, value);
}
static bool bounded_text(const char *text, size_t capacity) {
    return memchr(text, 0, capacity) != NULL && json_utf8(text);
}
tl_status ipc_encode(const tl_ipc_request *request, char *out, size_t capacity,
                     size_t *out_length) {
    if (out_length == NULL)
        return TL_INVALID;
    *out_length = 0;
    if (request == NULL ||
        (unsigned)request->operation >= sizeof(OPERATIONS) / sizeof(OPERATIONS[0]) ||
        request->request_id[0] == 0 ||
        !bounded_text(request->request_id, sizeof(request->request_id)) ||
        !bounded_text(request->query, sizeof(request->query)) ||
        !bounded_text(request->search_id, sizeof(request->search_id)) ||
        !bounded_text(request->event_id, sizeof(request->event_id)))
        return TL_INVALID;
    if (request->operation == IPC_QUERY &&
        (request->limit == 0 || request->limit > LEXICAL_MAX_RESULTS))
        return TL_INVALID;
    if ((request->operation == IPC_OPEN || request->operation == IPC_RESOLVE) &&
        (request->file_id == 0 || request->file_id > INT64_MAX))
        return TL_INVALID;
    if (request->operation == IPC_OPEN && request->event_id[0] == 0)
        return TL_INVALID;
    tl_json_buffer b;
    json_buffer_init(&b, out, capacity);
    json_raw(&b, "{\"version\":1");
    field(&b, "request_id", request->request_id);
    field(&b, "op", OPERATIONS[request->operation]);
    if (request->operation == IPC_QUERY) {
        field(&b, "query", request->query);
        json_raw(&b, ",\"limit\":");
        json_number(&b, request->limit);
    }
    if (request->operation == IPC_RESOLVE || request->operation == IPC_OPEN) {
        json_raw(&b, ",\"file_id\":\"");
        json_number(&b, request->file_id);
        json_raw(&b, "\"");
    }
    if (request->operation == IPC_OPEN) {
        field(&b, "event_id", request->event_id);
        if (request->search_id[0] != 0)
            field(&b, "search_id", request->search_id);
    }
    json_raw(&b, "}\n");
    if (b.status == TL_OK)
        *out_length = b.length;
    return b.status;
}
void ipc_result(tl_json_buffer *b, uint64_t id, const char *path) {
    json_raw(b, "{\"id\":\"");
    json_number(b, id);
    json_raw(b, "\",\"display\":");
    json_quote(b, path);
    json_raw(b, json_utf8(path) ? ",\"path\":" : ",\"path_b64\":");
    if (json_utf8(path))
        json_quote(b, path);
    else
        json_base64(b, path);
    json_raw(b, "}");
}
tl_status ipc_default_path(char **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    struct stat info;
    if (runtime == NULL || runtime[0] != '/')
        return TL_INVALID;
    if (stat(runtime, &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 0022) != 0)
        return TL_IO;
    size_t length = strlen(runtime);
    if (length > SIZE_MAX - sizeof("/torchlight.sock"))
        return TL_LIMIT;
    *out = malloc(length + sizeof("/torchlight.sock"));
    if (*out == NULL)
        return TL_NOMEM;
    memcpy(*out, runtime, length);
    memcpy(*out + length, "/torchlight.sock", sizeof("/torchlight.sock"));
    return TL_OK;
}
void ipc_path_destroy(char *path) {
    free(path);
}
tl_status ipc_lock(const char *path, int *out) {
    if (out == NULL)
        return TL_INVALID;
    *out = -1;
    if (path == NULL || path[0] != '/')
        return TL_INVALID;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return TL_IO;
    struct stat info;
    tl_status status = TL_OK;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 0077) != 0)
        status = TL_IO;
    if (status == TL_OK && flock(fd, LOCK_EX | LOCK_NB) != 0)
        status = errno == EWOULDBLOCK ? TL_STATE : TL_IO;
    if (status != TL_OK) {
        close(fd);
        return status;
    }
    *out = fd;
    return TL_OK;
}
void ipc_unlock(int fd) {
    if (fd >= 0)
        close(fd);
}
tl_status ipc_database_lock(const char *database, int *out) {
    if (out == NULL)
        return TL_INVALID;
    *out = -1;
    if (database == NULL)
        return TL_INVALID;
    size_t length = strlen(database);
    if (length > SIZE_MAX - sizeof(".daemon.lock"))
        return TL_LIMIT;
    char *path = malloc(length + sizeof(".daemon.lock"));
    if (path == NULL)
        return TL_NOMEM;
    memcpy(path, database, length);
    memcpy(path + length, ".daemon.lock", sizeof(".daemon.lock"));
    tl_status status = ipc_lock(path, out);
    free(path);
    return status;
}
static tl_status address(const char *path, struct sockaddr_un *out) {
    if (path == NULL || path[0] != '/')
        return TL_INVALID;
    size_t length = strlen(path);
    if (length >= sizeof(out->sun_path))
        return TL_LIMIT;
    memset(out, 0, sizeof(*out));
    out->sun_family = AF_UNIX;
    memcpy(out->sun_path, path, length + 1);
    return TL_OK;
}
static tl_status remove_stale_socket(const char *path, const struct sockaddr_un *socket_address) {
    struct stat info;
    if (lstat(path, &info) != 0)
        return errno == ENOENT ? TL_OK : TL_IO;
    if (!S_ISSOCK(info.st_mode) || info.st_uid != getuid())
        return TL_IO;
    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (probe < 0)
        return TL_IO;
    int code = connect(probe, (const struct sockaddr *)socket_address, sizeof(*socket_address));
    int error = errno;
    close(probe);
    if (code == 0 || error == EINPROGRESS || error == EAGAIN)
        return TL_STATE;
    if (error != ECONNREFUSED && error != ENOENT)
        return TL_IO;
    return unlink(path) == 0 || errno == ENOENT ? TL_OK : TL_IO;
}
tl_status ipc_listener_create(const char *path, tl_ipc_listener **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    struct sockaddr_un socket_address;
    tl_status status = address(path, &socket_address);
    if (status != TL_OK)
        return status;
    tl_ipc_listener *listener = calloc(1, sizeof(*listener));
    if (listener == NULL)
        return TL_NOMEM;
    listener->fd = -1;
    listener->lock = -1;
    listener->path = strdup(path);
    char lock_path[sizeof(socket_address.sun_path) + 6];
    size_t length = strlen(path);
    memcpy(lock_path, path, length);
    memcpy(lock_path + length, ".lock", 6);
    status = listener->path == NULL ? TL_NOMEM : ipc_lock(lock_path, &listener->lock);
    if (status == TL_OK)
        status = remove_stale_socket(path, &socket_address);
    if (status == TL_OK) {
        listener->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listener->fd < 0 || bind(listener->fd, (const struct sockaddr *)&socket_address,
                                     sizeof(socket_address)) != 0)
            status = TL_IO;
        else
            listener->bound = true;
    }
    if (status == TL_OK && (chmod(path, 0600) != 0 || listen(listener->fd, 16) != 0))
        status = TL_IO;
    if (status != TL_OK) {
        ipc_listener_destroy(listener);
        return status;
    }
    *out = listener;
    return TL_OK;
}
void ipc_listener_destroy(tl_ipc_listener *listener) {
    if (listener == NULL)
        return;
    if (listener->fd >= 0)
        close(listener->fd);
    if (listener->bound) {
        int code = unlink(listener->path);
        (void)code;
    }
    ipc_unlock(listener->lock);
    free(listener->path);
    free(listener);
}
int ipc_listener_descriptor(const tl_ipc_listener *listener) {
    return listener == NULL ? -1 : listener->fd;
}
tl_status ipc_accept(tl_ipc_listener *listener, int *out) {
    if (listener == NULL || out == NULL)
        return TL_INVALID;
    *out = -1;
    int fd = accept4(listener->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? TL_STATE : TL_IO;
    struct ucred peer;
    socklen_t length = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 || peer.uid != getuid()) {
        close(fd);
        return TL_IO;
    }
    *out = fd;
    return TL_OK;
}
static int64_t monotonic_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static tl_status ready(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t now = monotonic_ms();
        if (now < 0 || now >= deadline)
            return TL_IO;
        struct pollfd descriptor = {fd, events, 0};
        int code = poll(&descriptor, 1, (int)(deadline - now));
        if (code < 0 && errno == EINTR)
            continue;
        return code > 0 && (descriptor.revents & events) != 0 ? TL_OK : TL_IO;
    }
}
static tl_status connect_client(const char *path, int *out, int64_t deadline) {
    struct sockaddr_un socket_address;
    tl_status status = address(path, &socket_address);
    if (status != TL_OK)
        return status;
    *out = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (*out < 0)
        return TL_IO;
    if (connect(*out, (const struct sockaddr *)&socket_address, sizeof(socket_address)) != 0) {
        if (errno != EINPROGRESS)
            return TL_IO;
        status = ready(*out, POLLOUT, deadline);
        int error = 0;
        socklen_t length = sizeof(error);
        if (status == TL_OK &&
            (getsockopt(*out, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0))
            status = TL_IO;
    }
    struct ucred peer;
    socklen_t length = sizeof(peer);
    if (status == TL_OK &&
        (getsockopt(*out, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 || peer.uid != getuid()))
        status = TL_IO;
    return status;
}
tl_status ipc_call(const char *path, const tl_ipc_request *request, char *response, size_t capacity,
                   size_t *out_length) {
    if (out_length == NULL)
        return TL_INVALID;
    *out_length = 0;
    if (response == NULL || capacity < 2)
        return TL_INVALID;
    char line[IPC_REQUEST_BYTES];
    size_t length = 0;
    tl_status status = ipc_encode(request, line, sizeof(line), &length);
    int fd = -1;
    int64_t now = monotonic_ms(), deadline = now + IPC_DEADLINE_MS;
    if (now < 0)
        return TL_IO;
    if (status == TL_OK)
        status = connect_client(path, &fd, deadline);
    size_t sent = 0, received = 0;
    while (status == TL_OK && sent < length) {
        status = ready(fd, POLLOUT, deadline);
        if (status != TL_OK)
            break;
        ssize_t count = send(fd, line + sent, length - sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (count <= 0)
            status = TL_IO;
        else
            sent += (size_t)count;
    }
    while (status == TL_OK) {
        if (received + 1 == capacity) {
            status = TL_LIMIT;
            break;
        }
        status = ready(fd, POLLIN, deadline);
        if (status != TL_OK)
            break;
        ssize_t count = recv(fd, response + received, capacity - received - 1, 0);
        if (count < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (count <= 0) {
            status = TL_IO;
            break;
        }
        received += (size_t)count;
        response[received] = 0;
        char *newline = memchr(response, '\n', received);
        if (newline != NULL) {
            *out_length = (size_t)(newline - response) + 1;
            break;
        }
    }
    if (fd >= 0)
        close(fd);
    return status;
}
