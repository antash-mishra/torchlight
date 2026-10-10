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
    CHECK(watch_create(4, &watch) == TL_OK && watch_add(watch, root) == TL_OK);
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
void test_watch(void) {
    metadata_events();
    filesystem_types();
    char root[] = "/tmp/torchlight-watch-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char old[256], renamed[256], fresh[256];
    CHECK(snprintf(old, sizeof(old), "%s/old", root) > 0 && mkdir(old, 0700) == 0);
    CHECK(snprintf(renamed, sizeof(renamed), "%s/new", root) > 0);
    CHECK(snprintf(fresh, sizeof(fresh), "%s/new/fresh", root) > 0);
    tl_watch *watch = NULL;
    CHECK(watch_create(2, &watch) == TL_OK);
    CHECK(watch_add(watch, root) == TL_OK && watch_add(watch, old) == TL_OK &&
          watch_add(watch, old) == TL_OK);
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
    CHECK(watch_create(1, &watch) == TL_OK && watch_add(watch, root) == TL_OK);
    CHECK(watch_add(watch, renamed) == TL_LIMIT && watch_stats(watch).unavailable == 1);
    watch_destroy(watch);
    CHECK(unlink(fresh) == 0 && rmdir(renamed) == 0 && rmdir(arrived) == 0 && rmdir(root) == 0);
}
