/* fts-based physical scans report unreadable scopes separately from excluded trees. */
#include "torchlight/crawl.h"
#include <errno.h>
#include <fts.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
struct tl_crawl {
    char *exclude;
};
tl_status crawl_create(const char *exclude, tl_crawl **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    tl_crawl *crawler = calloc(1, sizeof(*crawler));
    if (crawler == NULL)
        return TL_NOMEM;
    if (exclude != NULL) {
        crawler->exclude = strdup(exclude);
        if (crawler->exclude == NULL) {
            free(crawler);
            return TL_NOMEM;
        }
    }
    *out = crawler;
    return TL_OK;
}
void crawl_destroy(tl_crawl *crawler) {
    if (crawler == NULL)
        return;
    free(crawler->exclude);
    free(crawler);
}
static bool within(const char *path, const char *scope) {
    if (scope == NULL)
        return false;
    size_t length = strlen(scope);
    return strncmp(path, scope, length) == 0 &&
           (path[length] == 0 || path[length] == '/' || (length == 1 && scope[0] == '/'));
}
static bool ignored(const FTSENT *entry) {
    if (entry->fts_level == 0 || (entry->fts_info != FTS_D && entry->fts_info != FTS_DNR))
        return false;
    const char *name = entry->fts_name;
    return name[0] == '.' || strcmp(name, "node_modules") == 0 || strcmp(name, "target") == 0 ||
           strcmp(name, "build") == 0 || strcmp(name, "__pycache__") == 0;
}
static bool failed(const FTSENT *entry) {
    return entry->fts_info == FTS_DNR || entry->fts_info == FTS_ERR || entry->fts_info == FTS_NS;
}
static tl_status visit(tl_crawl *crawler, FTS *walk, FTSENT *entry, tl_crawl_callback callback,
                       void *context) {
    if (entry->fts_info == FTS_DP)
        return TL_OK;
    if (within(entry->fts_path, crawler->exclude) || ignored(entry)) {
        if (entry->fts_info == FTS_D && fts_set(walk, entry, FTS_SKIP) != 0)
            return TL_IO;
        return TL_OK;
    }
    /* Without a readable root nothing was scanned, so the caller must not prune. */
    if (entry->fts_level == 0 && failed(entry))
        return TL_IO;
    /* An unreadable directory still has stat data; FTS_NS/FTS_ERR have none. A
     * directory cycle (only possible through bind mounts) is reported but fts
     * does not descend into it again. */
    bool has_stat = entry->fts_info != FTS_NS && entry->fts_info != FTS_ERR;
    if (has_stat && entry->fts_statp == NULL)
        return TL_IO;
    tl_crawl_entry record = {.path = entry->fts_path,
                             .is_dir = entry->fts_info == FTS_D || entry->fts_info == FTS_DNR ||
                                       entry->fts_info == FTS_DC,
                             .is_root = entry->fts_level == 0,
                             .unreadable = failed(entry),
                             .has_stat = has_stat,
                             .mtime = has_stat ? (int64_t)entry->fts_statp->st_mtime : 0,
                             .size = has_stat ? (int64_t)entry->fts_statp->st_size : 0};
    return callback(context, &record);
}
tl_status crawl_run(tl_crawl *crawler, const char *root, tl_crawl_callback callback,
                    void *context) {
    if (crawler == NULL || root == NULL || callback == NULL)
        return TL_INVALID;
    char *canonical = realpath(root, NULL);
    if (canonical == NULL)
        return TL_IO;
    /* NOCHDIR keeps callbacks and concurrent callers independent of cwd. */
    char *roots[] = {canonical, NULL};
    FTS *walk = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (walk == NULL) {
        free(canonical);
        return TL_IO;
    }
    tl_status status = TL_OK;
    for (;;) {
        errno = 0;
        FTSENT *entry = fts_read(walk);
        if (entry == NULL) {
            if (errno != 0)
                status = TL_IO;
            break;
        }
        status = visit(crawler, walk, entry, callback, context);
        if (status != TL_OK)
            break;
    }
    if (fts_close(walk) != 0 && status == TL_OK)
        status = TL_IO;
    free(canonical);
    return status;
}
