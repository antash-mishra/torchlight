/* inotify event collection with bounded watch/cookie storage and loss signals,
 * one long-lived watch set that full scans confirm and sweep instead of
 * rebuilding, plus the filesystem types whose changes inotify cannot fully
 * report. */
#include "torchlight/watch.h"
#include "torchlight/hashmap.h"
#include "torchlight/path.h"
#include "torchlight/vec.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/vfs.h>
#include <unistd.h>
#define WATCH_MOVE_SLOTS 256
#define WATCH_DRAIN_BYTES (256U * 1024U)
/* Names, places and directory permissions only: IN_CLOSE_WRITE/IN_MODIFY
 * would wake the writer for every save without changing what search reads. */
#define WATCH_MASK                                                                                 \
    (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_DELETE_SELF |            \
     IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW)
/* statfs f_type values (linux/magic.h) of filesystems whose changes by other
 * clients or the server never reach this kernel's inotify. */
#define WATCH_NFS_MAGIC 0x6969U
#define WATCH_SMB_MAGIC 0x517bU
#define WATCH_CIFS_MAGIC 0xff534d42U
#define WATCH_SMB2_MAGIC 0xfe534d42U
#define WATCH_FUSE_MAGIC 0x65735546U
#define WATCH_V9FS_MAGIC 0x01021997U
#define WATCH_CEPH_MAGIC 0x00c36400U
#define WATCH_AFS_MAGIC 0x5346414fU
#define WATCH_KAFS_MAGIC 0x6b414653U
#define WATCH_CODA_MAGIC 0x73757245U
/* One watched directory. identity is the object a crawl last saw at path
 * (known false until then); confirmed is the last scan generation that saw
 * it. listed: path is this slot's key in the path lookup. */
struct directory {
    int wd;
    char *path;
    tl_watch_identity identity;
    bool known, listed;
    uint32_t confirmed;
};
struct move {
    uint32_t cookie;
    char *path;
};
/* directories holds one slot per watch; a slot whose watch the kernel dropped
 * (wd -1, path NULL) is listed in free_slots and reused by the next watch_add,
 * so deleted directories never use up capacity. descriptors maps wd to slot;
 * paths maps a path to the one slot that currently owns it (a directory
 * moved away unnoticed keeps a stale path but loses the key to whatever is
 * watched there next). */
struct tl_watch {
    int fd;
    size_t capacity;
    tl_vec *directories;
    tl_vec *free_slots; /* uint32_t */
    tl_hashmap *descriptors, *paths;
    uint32_t generation;
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
struct path_lookup {
    const tl_watch *watch;
    const char *path;
};
static bool path_equal(const void *context, uint32_t value) {
    const struct path_lookup *key = context;
    const struct directory *entries = vec_const_data(key->watch->directories);
    return entries[value].path != NULL && strcmp(entries[value].path, key->path) == 0;
}
static uint64_t path_hash(const char *path) {
    return hashmap_hash(HASHMAP_HASH_SEED, path, strlen(path));
}
static bool find_path(const tl_watch *watch, const char *path, uint32_t *index) {
    struct path_lookup key = {watch, path};
    return hashmap_find(watch->paths, path_hash(path), path_equal, &key, index);
}
/* Drop slot index's key from the path lookup (before its path changes). */
static void unlist_path(tl_watch *watch, uint32_t index) {
    struct directory *entry = (struct directory *)vec_data(watch->directories) + index;
    if (!entry->listed)
        return;
    struct path_lookup key = {watch, entry->path};
    bool removed = hashmap_remove(watch->paths, path_hash(entry->path), path_equal, &key);
    (void)removed; /* listed slots are always in the lookup */
    entry->listed = false;
}
/* Make slot index the owner of its path, taking the key from a stale slot
 * that still names the same path. Without memory the slot stays reachable
 * by descriptor only, so lookups by path fall back to inotify_add_watch. */
static void list_path(tl_watch *watch, uint32_t index) {
    struct directory *entry = (struct directory *)vec_data(watch->directories) + index;
    uint32_t owner = 0;
    if (find_path(watch, entry->path, &owner)) {
        if (owner == index)
            return;
        unlist_path(watch, owner);
    }
    entry->listed = hashmap_insert(watch->paths, path_hash(entry->path), index) == TL_OK;
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
        status = vec_create(sizeof(uint32_t), &watch->free_slots);
    if (status == TL_OK)
        status = hashmap_create(&watch->descriptors);
    if (status == TL_OK)
        status = hashmap_create(&watch->paths);
    /* Generation 0 never confirms anything, so it can mean "not yet seen". */
    watch->generation = 1;
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
    vec_destroy(watch->free_slots);
    hashmap_destroy(watch->descriptors);
    hashmap_destroy(watch->paths);
    free(watch);
}
/* Store wd and a copy of path in a free slot, or append one. */
static tl_status claim_slot(tl_watch *watch, int wd, const char *path, uint32_t *index) {
    struct directory entry = {.wd = wd, .path = strdup(path)};
    if (entry.path == NULL)
        return TL_NOMEM;
    size_t free_count = vec_count(watch->free_slots);
    if (free_count != 0) {
        *index = ((const uint32_t *)vec_const_data(watch->free_slots))[free_count - 1];
        vec_truncate(watch->free_slots, free_count - 1);
        ((struct directory *)vec_data(watch->directories))[*index] = entry;
        return TL_OK;
    }
    *index = (uint32_t)vec_count(watch->directories);
    tl_status status = vec_append(watch->directories, &entry);
    if (status != TL_OK)
        free(entry.path);
    return status;
}
/* Empty a slot whose watch is gone and list it for reuse. A slot that cannot
 * be listed (no memory) just stays unused; capacity counts live watches. */
static void free_slot(tl_watch *watch, uint32_t index) {
    unlist_path(watch, index);
    struct directory *entries = vec_data(watch->directories);
    free(entries[index].path);
    entries[index] = (struct directory){.wd = -1};
    tl_status listed = vec_append(watch->free_slots, &index);
    (void)listed;
}
/* Remove slot index (holding a live wd) from both lookups and free it. */
static void drop_slot(tl_watch *watch, uint32_t index) {
    struct lookup key = {watch,
                         ((const struct directory *)vec_const_data(watch->directories))[index].wd};
    bool removed =
        hashmap_remove(watch->descriptors, (uint64_t)(unsigned)key.wd, descriptor_equal, &key);
    (void)removed; /* every live slot is in the descriptor lookup */
    free_slot(watch, index);
    if (watch->stats.directories != 0)
        watch->stats.directories--;
}
/* The kernel dropped a watch (its directory was deleted, moved to another
 * filesystem or unmounted): report the directory, then free its slot and
 * lookup entries, so directory churn between full scans cannot exhaust
 * capacity or grow the lookups. */
static tl_status forget(tl_watch *watch, int wd, tl_watch_callback callback, void *context) {
    struct lookup key = {watch, wd};
    uint32_t index = 0;
    if (!hashmap_find(watch->descriptors, (uint64_t)(unsigned)wd, descriptor_equal, &key, &index))
        return TL_OK;
    tl_watch_event ignored = {
        .path = ((const struct directory *)vec_const_data(watch->directories))[index].path};
    tl_status status = callback(context, &ignored);
    drop_slot(watch, index);
    return status;
}
/* Record that this scan saw the object identity (optional) at the slot's path. */
static void confirm(tl_watch *watch, struct directory *entry, const tl_watch_identity *identity) {
    entry->confirmed = watch->generation;
    if (identity != NULL) {
        entry->identity = *identity;
        entry->known = true;
    }
}
/* The kernel already watches this object under the slot's stored path: the
 * directory moved and its events are not drained yet, so adopt path. */
static tl_status adopt_path(tl_watch *watch, struct directory *entry, const char *path) {
    if (strcmp(entry->path, path) == 0)
        return TL_OK;
    char *copy = strdup(path);
    if (copy == NULL)
        return TL_NOMEM;
    uint32_t index = (uint32_t)(entry - (struct directory *)vec_data(watch->directories));
    unlist_path(watch, index);
    free(entry->path);
    entry->path = copy;
    list_path(watch, index);
    return TL_OK;
}
/* inotify_add_watch and a new slot for a directory the watcher lacks. */
static tl_status install(tl_watch *watch, int wd, const char *path,
                         const tl_watch_identity *identity) {
    /* Capacity bounds live watches; slots of dropped ones are reused. */
    tl_status status = watch->stats.directories >= watch->capacity ? TL_LIMIT : TL_OK;
    uint32_t index = 0;
    bool claimed = false;
    if (status == TL_OK) {
        status = claim_slot(watch, wd, path, &index);
        claimed = status == TL_OK;
    }
    if (status == TL_OK)
        status = hashmap_insert(watch->descriptors, (uint64_t)(unsigned)wd, index);
    if (status == TL_OK) {
        watch->stats.directories++;
        struct directory *entry = (struct directory *)vec_data(watch->directories) + index;
        confirm(watch, entry, identity);
        list_path(watch, index);
        return TL_OK;
    }
    /* Without a slot or a lookup entry its events could not be reported. */
    int code = inotify_rm_watch(watch->fd, wd);
    (void)code;
    if (claimed)
        free_slot(watch, index);
    if (status == TL_LIMIT)
        watch->stats.unavailable++;
    return status;
}
tl_status watch_add(tl_watch *watch, const char *path, const tl_watch_identity *identity) {
    if (watch == NULL || path == NULL || path[0] != '/')
        return TL_INVALID;
    /* The same object still at the same path: its watch is live (a deleted or
     * unmounted one would have been dropped), so no system call is needed. A
     * different object under a stale path (the old one moved away unnoticed)
     * falls through and gets its own watch. */
    uint32_t index = 0;
    if (identity != NULL && find_path(watch, path, &index)) {
        struct directory *known = (struct directory *)vec_data(watch->directories) + index;
        if (known->known && known->identity.device == identity->device &&
            known->identity.inode == identity->inode) {
            confirm(watch, known, identity);
            return TL_OK;
        }
    }
    int wd = inotify_add_watch(watch->fd, path, WATCH_MASK);
    if (wd < 0) {
        watch->stats.unavailable++;
        return TL_IO;
    }
    struct directory *existing = find_directory(watch, wd);
    if (existing == NULL)
        return install(watch, wd, path, identity);
    confirm(watch, existing, identity);
    return adopt_path(watch, existing, path);
}
void watch_begin(tl_watch *watch) {
    if (watch == NULL)
        return;
    /* Skip 0 on wraparound: it marks slots no generation confirmed. */
    watch->generation = watch->generation == UINT32_MAX ? 1 : watch->generation + 1;
    watch->stats.unavailable = 0;
}
size_t watch_sweep(tl_watch *watch, tl_watch_keep keep, void *context) {
    if (watch == NULL)
        return 0;
    size_t removed = 0;
    for (size_t i = 0; i < vec_count(watch->directories); i++) {
        const struct directory *entry = (const struct directory *)vec_data(watch->directories) + i;
        if (entry->path == NULL || entry->confirmed == watch->generation ||
            (keep != NULL && keep(context, entry->path)))
            continue;
        /* The kernel queues IN_IGNORED for this wd; with the slot gone,
         * dispatch finds no directory and drops it. */
        int code = inotify_rm_watch(watch->fd, entry->wd);
        (void)code;
        drop_slot(watch, (uint32_t)i);
        removed++;
    }
    return removed;
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
/* A paired directory move: every slot at or below old_path takes the new
 * prefix and counts as confirmed, since the directory still exists inside a
 * watched tree (a move during a full scan may skip the crawl of both paths). */
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
        unlist_path(watch, (uint32_t)i);
        free(entries[i].path);
        entries[i].path = path;
        entries[i].confirmed = watch->generation;
        list_path(watch, (uint32_t)i);
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
    if ((event->mask & IN_IGNORED) != 0)
        return forget(watch, event->wd, callback, context);
    char *path = NULL;
    tl_status status = event_path(directory, event, &path);
    if (status != TL_OK)
        return status;
    tl_watch_event update = {.path = path,
                             .is_dir = (event->mask & IN_ISDIR) != 0,
                             .metadata = (event->mask & (IN_ATTRIB | IN_CLOSE_WRITE)) != 0};
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
    update.created = (event->mask & IN_CREATE) != 0 ||
                     ((event->mask & IN_MOVED_TO) != 0 && paired == WATCH_MOVE_SLOTS);
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
bool watch_type_reliable(uint64_t filesystem_type) {
    static const uint64_t remote[] = {
        WATCH_NFS_MAGIC,  WATCH_SMB_MAGIC,  WATCH_CIFS_MAGIC, WATCH_SMB2_MAGIC, WATCH_FUSE_MAGIC,
        WATCH_V9FS_MAGIC, WATCH_CEPH_MAGIC, WATCH_AFS_MAGIC,  WATCH_KAFS_MAGIC, WATCH_CODA_MAGIC};
    for (size_t i = 0; i < sizeof(remote) / sizeof(remote[0]); i++)
        if (filesystem_type == remote[i])
            return false;
    return true;
}
tl_status watch_reliable(const char *path, bool *out) {
    if (path == NULL || out == NULL)
        return TL_INVALID;
    struct statfs info;
    if (statfs(path, &info) != 0)
        return TL_IO;
    /* f_type is a signed word; magic numbers are 32-bit patterns. */
    *out = watch_type_reliable((uint64_t)(uint32_t)info.f_type);
    return TL_OK;
}
