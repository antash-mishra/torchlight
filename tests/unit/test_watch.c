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
    bool paired, relocated;
};
static tl_status observed(void *context, const tl_watch_event *event) {
    struct observations *seen = context;
    seen->events++;
    if (event->overflow)
        seen->overflows++;
    if (event->old_path != NULL && strstr(event->old_path, "/old") != NULL &&
        strstr(event->path, "/new") != NULL)
        seen->paired = true;
    if (event->path != NULL && strstr(event->path, "/new/fresh") != NULL)
        seen->relocated = true;
    return TL_OK;
}
static void wait_events(tl_watch *watch, struct observations *seen) {
    struct pollfd fd = {watch_descriptor(watch), POLLIN, 0};
    CHECK(poll(&fd, 1, 1000) > 0);
    CHECK(watch_drain(watch, observed, seen) == TL_OK);
}
void test_watch(void) {
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
    CHECK(seen.relocated);
    struct inotify_event overflow = {.wd = -1, .mask = IN_Q_OVERFLOW};
    CHECK(watch_feed(watch, &overflow, sizeof(overflow), observed, &seen) == TL_OK &&
          seen.overflows == 1);
    CHECK(watch_feed(watch, &overflow, sizeof(overflow) - 1, observed, &seen) == TL_INVALID);
    CHECK(watch_stats(watch).overflows == 1);
    watch_destroy(watch);
    CHECK(watch_create(1, &watch) == TL_OK && watch_add(watch, root) == TL_OK);
    CHECK(watch_add(watch, renamed) == TL_LIMIT && watch_stats(watch).unavailable == 1);
    watch_destroy(watch);
    CHECK(unlink(fresh) == 0 && rmdir(renamed) == 0 && rmdir(root) == 0);
}
