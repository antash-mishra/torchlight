/* Deadline-bound subprocess capture; no shell, unbounded buffers or orphan workers. */
#include "torchlight/process.h"
#include "torchlight/vec.h"
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
uint64_t process_now(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0
               ? (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000
               : 0;
}
static tl_status remaining(const tl_process_budget *budget) {
    if (budget->cancel != NULL && atomic_load(budget->cancel))
        return TL_CANCELLED;
    return process_now() >= budget->deadline_ms ? TL_LIMIT : TL_OK;
}
static int spawn_child(const char *const *argv, int output[2], int errors[2], pid_t *pid) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    int code = posix_spawn_file_actions_init(&actions);
    if (code != 0)
        return code;
    code = posix_spawnattr_init(&attributes);
    if (code != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return code;
    }
    sigset_t empty, defaults;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGPIPE);
    code = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                                     POSIX_SPAWN_SETSIGDEF);
    if (code == 0)
        code = posix_spawnattr_setpgroup(&attributes, 0);
    if (code == 0)
        code = posix_spawnattr_setsigmask(&attributes, &empty);
    if (code == 0)
        code = posix_spawnattr_setsigdefault(&attributes, &defaults);
    if (code == 0)
        code = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (code == 0)
        code = posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    if (code == 0)
        code = posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
    char **env = g_get_environ();
    env = g_environ_setenv(env, "LC_ALL", "C.UTF-8", TRUE);
    if (code == 0)
        code = posix_spawnp(pid, argv[0], &actions, &attributes, (char *const *)argv, env);
    g_strfreev(env);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    return code;
}
static tl_status drain(int fd, bool capture, tl_vec *bytes, tl_process_budget *budget,
                       bool *closed) {
    char buffer[4096];
    ssize_t count = read(fd, buffer, sizeof(buffer));
    if (count == 0) {
        *closed = true;
        return TL_OK;
    }
    if (count < 0)
        return errno == EAGAIN || errno == EINTR ? TL_OK : TL_IO;
    size_t size = (size_t)count;
    if (size > budget->remaining_bytes)
        return TL_LIMIT;
    budget->remaining_bytes -= size;
    if (!capture)
        return TL_OK;
    if (memchr(buffer, 0, size) != NULL)
        return TL_INVALID;
    return vec_append_array(bytes, buffer, size);
}
static tl_status collect(pid_t pid, int output, int errors, tl_process_budget *budget,
                         tl_vec *bytes) {
    struct pollfd descriptors[] = {{output, POLLIN, 0}, {errors, POLLIN, 0}};
    bool closed[] = {false, false}, exited = false;
    int result = 0;
    tl_status status = TL_OK;
    while (status == TL_OK && (!closed[0] || !closed[1] || !exited)) {
        status = remaining(budget);
        if (status != TL_OK)
            break;
        if (poll(descriptors, 2, 20) < 0 && errno != EINTR) {
            status = TL_IO;
            break;
        }
        for (size_t i = 0; i < 2 && status == TL_OK; i++) {
            if (closed[i] || descriptors[i].revents == 0)
                continue;
            status = drain(descriptors[i].fd, i == 0, bytes, budget, &closed[i]);
            if (closed[i])
                descriptors[i].fd = -1;
        }
        if (!exited) {
            pid_t waited = waitpid(pid, &result, WNOHANG);
            exited = waited == pid;
            if (waited < 0 && errno != EINTR)
                status = TL_IO;
        }
    }
    if (status != TL_OK) {
        /* The group includes grandchildren holding the capture pipes open. */
        (void)kill(-pid, SIGKILL);
        if (!exited)
            while (waitpid(pid, &result, 0) < 0 && errno == EINTR) {
            }
        return status;
    }
    if (WIFEXITED(result) && WEXITSTATUS(result) == 0)
        return TL_OK;
    /* A failed leader can leave descendants that already closed the pipes. */
    (void)kill(-pid, SIGKILL);
    return TL_IO;
}
tl_status process_capture(const char *const *argv, tl_process_budget *budget, char **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (argv == NULL || argv[0] == NULL || budget == NULL)
        return TL_INVALID;
    tl_status status = remaining(budget);
    if (status != TL_OK)
        return status;
    int output[2] = {-1, -1}, errors[2] = {-1, -1};
    tl_vec *bytes = NULL;
    status = vec_create(1, &bytes);
    if (status == TL_OK && (pipe2(output, O_CLOEXEC) != 0 || pipe2(errors, O_CLOEXEC) != 0))
        status = TL_IO;
    pid_t pid = -1;
    if (status == TL_OK) {
        int code = spawn_child(argv, output, errors, &pid);
        if (code != 0)
            status = code == ENOENT ? TL_STATE : TL_IO;
    }
    if (output[1] >= 0)
        close(output[1]);
    if (errors[1] >= 0)
        close(errors[1]);
    if (status == TL_OK) {
        if (fcntl(output[0], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(errors[0], F_SETFL, O_NONBLOCK) < 0) {
            (void)kill(-pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
            }
            status = TL_IO;
        } else
            status = collect(pid, output[0], errors[0], budget, bytes);
    }
    if (output[0] >= 0)
        close(output[0]);
    if (errors[0] >= 0)
        close(errors[0]);
    if (status == TL_OK) {
        const char end = 0;
        status = vec_append(bytes, &end);
        if (status == TL_OK) {
            *out = strdup(vec_const_data(bytes));
            if (*out == NULL)
                status = TL_NOMEM;
        }
    }
    vec_destroy(bytes);
    return status;
}
void process_output_destroy(char *output) {
    free(output);
}
