/* inotify event collection with bounded watch/cookie storage and loss signals. */
#include "torchlight/watch.h"
#include "torchlight/hashmap.h"
#include "torchlight/path.h"
#include "torchlight/vec.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>
#define WATCH_MOVE_SLOTS 256
#define WATCH_DRAIN_BYTES (256U * 1024U)
struct directory {
    int wd;
    char *path;
};
struct move {
    uint32_t cookie;
    char *path;
};
struct tl_watch {
    int fd;
    size_t capacity;
    tl_vec *directories;
    tl_hashmap *descriptors;
    struct move moves[WATCH_MOVE_SLOTS];
    tl_watch_stats stats;
};
struct lookup {
    const tl_watch *watch;
    int wd;
};
static bool descriptor_equal(const void *context, uint32_t value) {
    const struct lookup *key = context;
    const struct directory *entries = vec_const_data(key->watch->directories);
    return entries[value].wd == key->wd;
}
static struct directory *find_directory(tl_watch *watch, int wd) {
    struct lookup key = {watch, wd};
    uint32_t index = 0;
    if (!hashmap_find(watch->descriptors, (uint64_t)(unsigned)wd, descriptor_equal, &key, &index))
        return NULL;
    return (struct directory *)vec_data(watch->directories) + index;
}
tl_status watch_create(size_t capacity, tl_watch **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (capacity == 0 || capacity > 1000000)
        return TL_INVALID;
    tl_watch *watch = calloc(1, sizeof(*watch));
    if (watch == NULL)
        return TL_NOMEM;
    watch->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    watch->capacity = capacity;
    tl_status status =
        watch->fd < 0 ? TL_IO : vec_create(sizeof(struct directory), &watch->directories);
    if (status == TL_OK)
        status = hashmap_create(&watch->descriptors);
    if (status != TL_OK) {
        watch_destroy(watch);
        return status;
    }
    *out = watch;
    return TL_OK;
}
void watch_clear_moves(tl_watch *watch) {
    if (watch == NULL)
        return;
    for (size_t i = 0; i < WATCH_MOVE_SLOTS; i++) {
        free(watch->moves[i].path);
        watch->moves[i] = (struct move){0};
    }
}
void watch_destroy(tl_watch *watch) {
    if (watch == NULL)
        return;
    if (watch->fd >= 0)
        close(watch->fd);
    struct directory *entries = vec_data(watch->directories);
    for (size_t i = 0; i < vec_count(watch->directories); i++)
        free(entries[i].path);
    watch_clear_moves(watch);
    vec_destroy(watch->directories);
    hashmap_destroy(watch->descriptors);
    free(watch);
}
tl_status watch_add(tl_watch *watch, const char *path) {
    if (watch == NULL || path == NULL || path[0] != '/')
        return TL_INVALID;
    int wd = inotify_add_watch(watch->fd, path,
                               IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
                                   IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR |
                                   IN_DONT_FOLLOW);
    if (wd < 0) {
        watch->stats.unavailable++;
        return TL_IO;
    }
    struct directory *existing = find_directory(watch, wd);
    if (existing != NULL)
        return TL_OK;
    if (vec_count(watch->directories) >= watch->capacity) {
        int code = inotify_rm_watch(watch->fd, wd);
        (void)code;
        watch->stats.unavailable++;
        return TL_LIMIT;
    }
    struct directory entry = {wd, strdup(path)};
    if (entry.path == NULL)
        return TL_NOMEM;
    uint32_t index = (uint32_t)vec_count(watch->directories);
    tl_status status = vec_append(watch->directories, &entry);
    if (status != TL_OK) {
        free(entry.path);
        return status;
    }
    status = hashmap_insert(watch->descriptors, (uint64_t)(unsigned)wd, index);
    if (status == TL_OK)
        watch->stats.directories++;
    return status;
}
static tl_status loss(tl_watch *watch, tl_watch_callback callback, void *context) {
    watch->stats.overflows++;
    watch_clear_moves(watch);
    tl_watch_event event = {.overflow = true};
    return callback(context, &event);
}
static tl_status event_path(const struct directory *directory, const struct inotify_event *event,
                            char **out) {
    size_t base = strlen(directory->path),
           name = event->len == 0 ? 0 : strnlen(event->name, event->len);
    if (base > SIZE_MAX - name - 2)
        return TL_LIMIT;
    *out = malloc(base + name + 2);
    if (*out == NULL)
        return TL_NOMEM;
    memcpy(*out, directory->path, base);
    if (name != 0) {
        if (base != 1)
            (*out)[base++] = '/';
        memcpy(*out + base, event->name, name);
    }
    (*out)[base + name] = 0;
    return TL_OK;
}
static tl_status remember_move(tl_watch *watch, const struct inotify_event *event, const char *path,
                               tl_watch_callback callback, void *context) {
    for (size_t i = 0; i < WATCH_MOVE_SLOTS; i++) {
        if (watch->moves[i].path != NULL)
            continue;
        watch->moves[i] = (struct move){event->cookie, strdup(path)};
        return watch->moves[i].path == NULL ? TL_NOMEM : TL_OK;
    }
    return loss(watch, callback, context);
}
static tl_status relocate(tl_watch *watch, const char *old_path, const char *new_path) {
    size_t old_length = strlen(old_path), new_length = strlen(new_path);
    struct directory *entries = vec_data(watch->directories);
    for (size_t i = 0; i < vec_count(watch->directories); i++) {
        if (!path_within(entries[i].path, old_path))
            continue;
        size_t suffix = strlen(entries[i].path) - old_length;
        if (new_length > SIZE_MAX - suffix - 1)
            return TL_LIMIT;
        char *path = malloc(new_length + suffix + 1);
        if (path == NULL)
            return TL_NOMEM;
        memcpy(path, new_path, new_length);
        memcpy(path + new_length, entries[i].path + old_length, suffix + 1);
        free(entries[i].path);
        entries[i].path = path;
    }
    return TL_OK;
}
static tl_status dispatch(tl_watch *watch, const struct inotify_event *event,
                          tl_watch_callback callback, void *context) {
    if ((event->mask & IN_Q_OVERFLOW) != 0)
        return loss(watch, callback, context);
    struct directory *directory = find_directory(watch, event->wd);
    if (directory == NULL)
        return TL_OK;
    if ((event->mask & IN_IGNORED) != 0) {
        directory->wd = -1;
        if (watch->stats.directories != 0)
            watch->stats.directories--;
        tl_watch_event ignored = {.path = directory->path};
        return callback(context, &ignored);
    }
    char *path = NULL;
    tl_status status = event_path(directory, event, &path);
    if (status != TL_OK)
        return status;
    tl_watch_event update = {.path = path, .is_dir = (event->mask & IN_ISDIR) != 0};
    size_t paired = WATCH_MOVE_SLOTS;
    if ((event->mask & IN_MOVED_FROM) != 0 && event->cookie != 0)
        status = remember_move(watch, event, path, callback, context);
    if ((event->mask & IN_MOVED_TO) != 0 && event->cookie != 0) {
        for (size_t i = 0; i < WATCH_MOVE_SLOTS; i++) {
            if (watch->moves[i].path != NULL && watch->moves[i].cookie == event->cookie) {
                paired = i;
                update.old_path = watch->moves[i].path;
                break;
            }
        }
    }
    if (status == TL_OK && paired != WATCH_MOVE_SLOTS && update.is_dir)
        status = relocate(watch, update.old_path, update.path);
    if (status == TL_OK)
        status = callback(context, &update);
    if (paired != WATCH_MOVE_SLOTS) {
        free(watch->moves[paired].path);
        watch->moves[paired] = (struct move){0};
    }
    free(path);
    return status;
}
tl_status watch_feed(tl_watch *watch, const void *bytes, size_t length, tl_watch_callback callback,
                     void *context) {
    if (watch == NULL || bytes == NULL || callback == NULL)
        return TL_INVALID;
    const unsigned char *data = bytes;
    for (size_t offset = 0; offset < length;) {
        struct inotify_event header;
        if (length - offset < sizeof(header))
            return TL_INVALID;
        memcpy(&header, data + offset, sizeof(header));
        if (header.len > length - offset - sizeof(header))
            return TL_INVALID;
        /* Kernel events are aligned. Test input must meet the same contract. */
        if (offset % _Alignof(struct inotify_event) != 0 ||
            (uintptr_t)(data + offset) % _Alignof(struct inotify_event) != 0)
            return TL_INVALID;
        const struct inotify_event *event =
            (const struct inotify_event *)(const void *)(data + offset);
        if (header.len != 0 && (memchr(event->name, 0, header.len) == NULL ||
                                memchr(event->name, '/', strnlen(event->name, header.len)) != NULL))
            return TL_INVALID;
        tl_status status = dispatch(watch, event, callback, context);
        if (status != TL_OK)
            return status;
        offset += sizeof(header) + header.len;
    }
    return TL_OK;
}
tl_status watch_drain(tl_watch *watch, tl_watch_callback callback, void *context) {
    if (watch == NULL || callback == NULL)
        return TL_INVALID;
    union {
        struct inotify_event alignment;
        unsigned char bytes[16384];
    } buffer;
    for (size_t total = 0; total < WATCH_DRAIN_BYTES;) {
        ssize_t count = read(watch->fd, buffer.bytes, sizeof(buffer.bytes));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno == EAGAIN)
            return TL_OK;
        if (count <= 0)
            return TL_IO;
        tl_status status = watch_feed(watch, buffer.bytes, (size_t)count, callback, context);
        if (status != TL_OK)
            return status;
        total += (size_t)count;
    }
    return TL_OK;
}
int watch_descriptor(const tl_watch *watch) {
    return watch == NULL ? -1 : watch->fd;
}
tl_watch_stats watch_stats(const tl_watch *watch) {
    if (watch == NULL) {
        tl_watch_stats empty = {0};
        return empty;
    }
    return watch->stats;
}
