/* Real rename cookies, relocated directory watches and deterministic event loss. */
#include "test.h"
#include "torchlight/watch.h"
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
struct observations {
    size_t events, overflows;
    bool paired, relocated, paired_created, fresh_created, arrived_created;
};
static tl_status observed(void *context, const tl_watch_event *event) {
    struct observations *seen = context;
    seen->events++;
    if (event->overflow)
        seen->overflows++;
    if (event->old_path != NULL && strstr(event->old_path, "/old") != NULL &&
        strstr(event->path, "/new") != NULL) {
        seen->paired = true;
        seen->paired_created = seen->paired_created || event->created;
    }
    if (event->path != NULL && strstr(event->path, "/new/fresh") != NULL) {
        seen->relocated = true;
        seen->fresh_created = seen->fresh_created || event->created;
    }
    if (event->path != NULL && strstr(event->path, "/arrived") != NULL)
        seen->arrived_created = seen->arrived_created || event->created;
    return TL_OK;
}
static void wait_events(tl_watch *watch, struct observations *seen) {
    struct pollfd fd = {watch_descriptor(watch), POLLIN, 0};
    CHECK(poll(&fd, 1, 1000) > 0);
    CHECK(watch_drain(watch, observed, seen) == TL_OK);
}
struct metadata_seen {
    size_t events;
    bool file, directory, other;
};
static tl_status observe_metadata(void *context, const tl_watch_event *event) {
    struct metadata_seen *seen = context;
    seen->events++;
    if (event->metadata && !event->is_dir && strstr(event->path, "/file.txt") != NULL)
        seen->file = true;
    else if (event->metadata && event->is_dir && strstr(event->path, "/sub") != NULL)
        seen->directory = true;
    else
        seen->other = true;
    return TL_OK;
}
/* Attribute changes leave names alone and say so; writes are not subscribed
 * at all, but a fed close-write still classifies as metadata. */
static void metadata_events(void) {
    char root[] = "/tmp/torchlight-watch-meta-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char file[256], sub[256];
    CHECK(snprintf(file, sizeof(file), "%s/file.txt", root) > 0);
    CHECK(snprintf(sub, sizeof(sub), "%s/sub", root) > 0 && mkdir(sub, 0700) == 0);
    int fd = open(file, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    tl_watch *watch = NULL;
    CHECK(watch_create(4, &watch) == TL_OK && watch_add(watch, root, NULL) == TL_OK);
    CHECK(chmod(file, 0644) == 0 && chmod(sub, 0755) == 0);
    struct metadata_seen seen = {0};
    struct pollfd ready = {watch_descriptor(watch), POLLIN, 0};
    while (!(seen.file && seen.directory) && poll(&ready, 1, 1000) > 0)
        CHECK(watch_drain(watch, observe_metadata, &seen) == TL_OK);
    CHECK(seen.file && seen.directory && !seen.other);
    fd = open(file, O_WRONLY | O_APPEND);
    CHECK(fd >= 0 && write(fd, "x", 1) == 1 && close(fd) == 0);
    CHECK(poll(&ready, 1, 100) == 0); /* no close-write event */
    /* The first watch of a fresh instance has descriptor 1. */
    union {
        struct inotify_event event;
        unsigned char bytes[sizeof(struct inotify_event) + 16];
    } closed = {.event = {.wd = 1, .mask = IN_CLOSE_WRITE, .len = 16}};
    memcpy(closed.event.name, "file.txt", sizeof("file.txt"));
    seen = (struct metadata_seen){0};
    CHECK(watch_feed(watch, closed.bytes, sizeof(closed.bytes), observe_metadata, &seen) == TL_OK);
    CHECK(seen.events == 1 && seen.file);
    watch_destroy(watch);
    CHECK(unlink(file) == 0 && rmdir(sub) == 0 && rmdir(root) == 0);
}
/* statfs f_type values (linux/magic.h). */
#define EXT4_MAGIC 0xef53U
#define TMPFS_MAGIC 0x01021994U
#define BTRFS_MAGIC 0x9123683eU
#define NFS_MAGIC 0x6969U
#define FUSE_MAGIC 0x65735546U
#define CIFS_MAGIC 0xff534d42U
#define SMB2_MAGIC 0xfe534d42U
#define V9FS_MAGIC 0x01021997U
static void filesystem_types(void) {
    CHECK(watch_type_reliable(EXT4_MAGIC) && watch_type_reliable(TMPFS_MAGIC) &&
          watch_type_reliable(BTRFS_MAGIC));
    CHECK(!watch_type_reliable(NFS_MAGIC) && !watch_type_reliable(FUSE_MAGIC) &&
          !watch_type_reliable(CIFS_MAGIC) && !watch_type_reliable(SMB2_MAGIC) &&
          !watch_type_reliable(V9FS_MAGIC));
    bool reliable = false;
    CHECK(watch_reliable("/proc", &reliable) == TL_OK && reliable);
    CHECK(watch_reliable("/torchlight-missing-path", &reliable) == TL_IO);
    CHECK(watch_reliable(NULL, &reliable) == TL_INVALID && watch_reliable("/", NULL) == TL_INVALID);
}
/* Directory create/delete cycles with room for the root plus one more. */
#define SLOT_CYCLES 5
struct slot_events {
    bool file_seen;
    const char *file;
};
static tl_status observe_slot(void *context, const tl_watch_event *event) {
    struct slot_events *seen = context;
    if (event->path != NULL && strcmp(event->path, seen->file) == 0)
        seen->file_seen = true;
    return TL_OK;
}
/* Drain until the watcher holds wanted live watches (bounded). */
static void drain_until(tl_watch *watch, size_t wanted, struct slot_events *seen) {
    struct pollfd fd = {watch_descriptor(watch), POLLIN, 0};
    for (size_t i = 0; i < 100 && watch_stats(watch).directories != wanted; i++)
        if (poll(&fd, 1, 100) > 0)
            CHECK(watch_drain(watch, observe_slot, seen) == TL_OK);
    CHECK(watch_stats(watch).directories == wanted);
}
/* M7: the kernel drops a deleted directory's watch (IN_IGNORED) and its slot
 * is reused, so directory churn between full scans never exhausts capacity
 * (each cycle after the first failed with TL_LIMIT before), and a reused
 * slot reports events under its new path. */
static void slot_reuse(void) {
    char root[] = "/tmp/torchlight-slots-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char directory[256], file[300];
    struct slot_events seen = {false, file};
    CHECK(snprintf(file, sizeof(file), "%s/none", root) > 0);
    tl_watch *watch = NULL;
    CHECK(watch_create(2, &watch) == TL_OK && watch_add(watch, root, NULL) == TL_OK);
    for (int i = 0; i < SLOT_CYCLES; i++) {
        CHECK(snprintf(directory, sizeof(directory), "%s/d%d", root, i) > 0);
        CHECK(mkdir(directory, 0700) == 0 && watch_add(watch, directory, NULL) == TL_OK);
        CHECK(rmdir(directory) == 0);
        drain_until(watch, 1, &seen);
    }
    CHECK(snprintf(directory, sizeof(directory), "%s/last", root) > 0);
    CHECK(snprintf(file, sizeof(file), "%s/file", directory) > 0);
    CHECK(mkdir(directory, 0700) == 0 && watch_add(watch, directory, NULL) == TL_OK);
    int fd = open(file, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    struct pollfd ready = {watch_descriptor(watch), POLLIN, 0};
    for (size_t i = 0; i < 100 && !seen.file_seen; i++)
        if (poll(&ready, 1, 100) > 0)
            CHECK(watch_drain(watch, observe_slot, &seen) == TL_OK);
    CHECK(seen.file_seen && watch_stats(watch).unavailable == 0);
    watch_destroy(watch);
    CHECK(unlink(file) == 0 && rmdir(directory) == 0 && rmdir(root) == 0);
}
static tl_watch_identity identity_of(const char *path) {
    struct stat info;
    CHECK(stat(path, &info) == 0);
    return (tl_watch_identity){(uint64_t)info.st_dev, (uint64_t)info.st_ino};
}
struct swept_events {
    const char *wanted, *unwanted;
    bool wanted_seen, unwanted_seen;
};
static tl_status observe_needles(void *context, const tl_watch_event *event) {
    struct swept_events *seen = context;
    if (event->path != NULL && strstr(event->path, seen->wanted) != NULL)
        seen->wanted_seen = true;
    if (event->path != NULL && strstr(event->path, seen->unwanted) != NULL)
        seen->unwanted_seen = true;
    return TL_OK;
}
/* Drain until an event path contains wanted (at most a second), then 100 ms
 * more; true when wanted arrived and unwanted never did. */
static bool only_wanted(tl_watch *watch, const char *wanted, const char *unwanted) {
    struct swept_events seen = {wanted, unwanted, false, false};
    struct pollfd ready = {watch_descriptor(watch), POLLIN, 0};
    while (!seen.wanted_seen && poll(&ready, 1, 1000) > 0)
        CHECK(watch_drain(watch, observe_needles, &seen) == TL_OK);
    while (poll(&ready, 1, 100) > 0)
        CHECK(watch_drain(watch, observe_needles, &seen) == TL_OK);
    return seen.wanted_seen && !seen.unwanted_seen;
}
static void touch_file(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
}
/* ADR 0040: a full scan re-adding a directory the watcher already holds for
 * the same object makes no system call. Proof: with the directory unreadable,
 * inotify_add_watch fails, yet the confirmed add succeeds. */
static void confirmed_watches_skip_the_kernel(void) {
    if (geteuid() == 0)
        return; /* root can watch mode-0 directories */
    char root[] = "/tmp/torchlight-confirm-XXXXXX", sub[256];
    CHECK(mkdtemp(root) != NULL);
    CHECK(snprintf(sub, sizeof(sub), "%s/sub", root) > 0 && mkdir(sub, 0700) == 0);
    tl_watch_identity root_identity = identity_of(root), sub_identity = identity_of(sub);
    tl_watch *watch = NULL;
    CHECK(watch_create(4, &watch) == TL_OK);
    CHECK(watch_add(watch, root, &root_identity) == TL_OK &&
          watch_add(watch, sub, &sub_identity) == TL_OK);
    CHECK(chmod(sub, 0) == 0);
    watch_begin(watch);
    CHECK(watch_add(watch, root, &root_identity) == TL_OK &&
          watch_add(watch, sub, &sub_identity) == TL_OK);
    CHECK(watch_stats(watch).unavailable == 0 && watch_sweep(watch, NULL, NULL) == 0);
    CHECK(watch_stats(watch).directories == 2);
    /* Without identity the kernel is asked, and refuses. */
    CHECK(watch_add(watch, sub, NULL) == TL_IO && watch_stats(watch).unavailable == 1);
    watch_begin(watch);
    CHECK(watch_stats(watch).unavailable == 0);
    watch_destroy(watch);
    CHECK(chmod(sub, 0700) == 0 && rmdir(sub) == 0 && rmdir(root) == 0);
}
static bool keep_nothing(void *context, const char *path) {
    (void)context;
    (void)path;
    return false;
}
static bool keep_all(void *context, const char *path) {
    (void)context;
    (void)path;
    return true;
}
/* A full scan that no longer reaches a directory (excluded, deleted, moved
 * out of every root) removes its watch; keep spares unscanned roots. */
static void sweep_drops_unconfirmed(void) {
    char root[] = "/tmp/torchlight-sweep-XXXXXX", sub[256], inner[300], outer[300];
    CHECK(mkdtemp(root) != NULL);
    CHECK(snprintf(sub, sizeof(sub), "%s/sub", root) > 0 && mkdir(sub, 0700) == 0);
    CHECK(snprintf(inner, sizeof(inner), "%s/inner.txt", sub) > 0);
    CHECK(snprintf(outer, sizeof(outer), "%s/outer.txt", root) > 0);
    tl_watch_identity root_identity = identity_of(root), sub_identity = identity_of(sub);
    tl_watch *watch = NULL;
    CHECK(watch_create(4, &watch) == TL_OK);
    CHECK(watch_add(watch, root, &root_identity) == TL_OK &&
          watch_add(watch, sub, &sub_identity) == TL_OK);
    watch_begin(watch);
    CHECK(watch_add(watch, root, &root_identity) == TL_OK);
    CHECK(watch_sweep(watch, keep_all, NULL) == 0 && watch_stats(watch).directories == 2);
    CHECK(watch_sweep(watch, keep_nothing, NULL) == 1 && watch_stats(watch).directories == 1);
    touch_file(inner);
    touch_file(outer);
    CHECK(only_wanted(watch, "/outer.txt", "/inner.txt"));
    /* Confirming again after a sweep re-adds the directory. */
    watch_begin(watch);
    CHECK(watch_add(watch, root, &root_identity) == TL_OK &&
          watch_add(watch, sub, &sub_identity) == TL_OK);
    CHECK(watch_sweep(watch, NULL, NULL) == 0 && watch_stats(watch).directories == 2);
    watch_destroy(watch);
    CHECK(unlink(inner) == 0 && unlink(outer) == 0 && rmdir(sub) == 0 && rmdir(root) == 0);
}
/* A directory moved out of the watched tree keeps its watch under the old
 * path (only IN_MOVE_SELF reaches it). A new directory created at that path
 * must get its own watch, not be mistaken for the old one; the sweep then
 * drops the stale watch. */
static void stale_path_gets_new_watch(void) {
    char root[] = "/tmp/torchlight-stale-XXXXXX", outside[] = "/tmp/torchlight-away-XXXXXX";
    char path[256], away[256], file[300], gone[300];
    CHECK(mkdtemp(root) != NULL && mkdtemp(outside) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/a", root) > 0 && mkdir(path, 0700) == 0);
    CHECK(snprintf(away, sizeof(away), "%s/a", outside) > 0);
    CHECK(snprintf(file, sizeof(file), "%s/file.txt", path) > 0);
    CHECK(snprintf(gone, sizeof(gone), "%s/gone.txt", away) > 0);
    tl_watch_identity root_identity = identity_of(root), old_identity = identity_of(path);
    tl_watch *watch = NULL;
    CHECK(watch_create(4, &watch) == TL_OK);
    CHECK(watch_add(watch, root, &root_identity) == TL_OK &&
          watch_add(watch, path, &old_identity) == TL_OK);
    CHECK(rename(path, away) == 0 && mkdir(path, 0700) == 0);
    tl_watch_identity new_identity = identity_of(path);
    CHECK(new_identity.inode != old_identity.inode);
    watch_begin(watch);
    CHECK(watch_add(watch, root, &root_identity) == TL_OK &&
          watch_add(watch, path, &new_identity) == TL_OK);
    CHECK(watch_stats(watch).directories == 3);
    CHECK(watch_sweep(watch, NULL, NULL) == 1 && watch_stats(watch).directories == 2);
    touch_file(gone);
    touch_file(file);
    CHECK(only_wanted(watch, "/a/file.txt", "/a/gone.txt"));
    watch_destroy(watch);
    CHECK(unlink(file) == 0 && unlink(gone) == 0 && rmdir(path) == 0 && rmdir(away) == 0);
    CHECK(rmdir(root) == 0 && rmdir(outside) == 0);
}
void test_watch(void) {
    confirmed_watches_skip_the_kernel();
    sweep_drops_unconfirmed();
    stale_path_gets_new_watch();
    metadata_events();
    filesystem_types();
    slot_reuse();
    char root[] = "/tmp/torchlight-watch-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char old[256], renamed[256], fresh[256];
    CHECK(snprintf(old, sizeof(old), "%s/old", root) > 0 && mkdir(old, 0700) == 0);
    CHECK(snprintf(renamed, sizeof(renamed), "%s/new", root) > 0);
    CHECK(snprintf(fresh, sizeof(fresh), "%s/new/fresh", root) > 0);
    tl_watch *watch = NULL;
    CHECK(watch_create(2, &watch) == TL_OK);
    CHECK(watch_add(watch, root, NULL) == TL_OK && watch_add(watch, old, NULL) == TL_OK &&
          watch_add(watch, old, NULL) == TL_OK);
    CHECK(watch_stats(watch).directories == 2);
    CHECK(rename(old, renamed) == 0);
    struct observations seen = {0};
    wait_events(watch, &seen);
    CHECK(seen.paired);
    int fd = open(fresh, O_CREAT | O_WRONLY, 0600);
    CHECK(fd >= 0 && close(fd) == 0);
    wait_events(watch, &seen);
    CHECK(seen.relocated && seen.fresh_created && !seen.paired_created);
    /* A directory moved in from an unwatched place has no paired source: its
     * subtree is new to the watcher. */
    char outside[] = "/tmp/torchlight-watch-out-XXXXXX", arrived[256];
    CHECK(mkdtemp(outside) != NULL);
    CHECK(snprintf(arrived, sizeof(arrived), "%s/arrived", root) > 0);
    CHECK(rename(outside, arrived) == 0);
    wait_events(watch, &seen);
    CHECK(seen.arrived_created);
    struct inotify_event overflow = {.wd = -1, .mask = IN_Q_OVERFLOW};
    CHECK(watch_feed(watch, &overflow, sizeof(overflow), observed, &seen) == TL_OK &&
          seen.overflows == 1);
    CHECK(watch_feed(watch, &overflow, sizeof(overflow) - 1, observed, &seen) == TL_INVALID);
    CHECK(watch_stats(watch).overflows == 1);
    watch_destroy(watch);
    CHECK(watch_create(1, &watch) == TL_OK && watch_add(watch, root, NULL) == TL_OK);
    CHECK(watch_add(watch, renamed, NULL) == TL_LIMIT && watch_stats(watch).unavailable == 1);
    watch_destroy(watch);
    CHECK(unlink(fresh) == 0 && rmdir(renamed) == 0 && rmdir(arrived) == 0 && rmdir(root) == 0);
}
