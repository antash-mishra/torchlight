/* fts-based physical scans: default exclusions, allowlist traversal, and
 * unreadable scopes reported separately from excluded trees. */
#include "torchlight/crawl.h"
#include "torchlight/path.h"
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
struct tl_crawl {
    char *exclude;
    char **allow;
    size_t allow_count;
};
/* What a walk does with a path: report it, pass through it to reach an
 * allowlisted directory below, or leave it (and its subtree) alone. */
enum scope { SCOPE_INDEX, SCOPE_TRANSIT, SCOPE_SKIP };
/* A full root walk, a scoped walk of one indexed directory's whole subtree,
 * or of that directory and its direct children only. */
enum walk_mode { WALK_ROOT, WALK_TREE, WALK_CHILDREN };
tl_status crawl_create(const char *exclude, const char *const *allow, size_t allow_count,
                       tl_crawl **out) {
    if (out == NULL || (allow == NULL && allow_count != 0))
        return TL_INVALID;
    *out = NULL;
    for (size_t i = 0; i < allow_count; i++) {
        if (allow[i] == NULL)
            return TL_INVALID;
    }
    tl_crawl *crawler = calloc(1, sizeof(*crawler));
    if (crawler == NULL)
        return TL_NOMEM;
    crawler->allow = calloc(allow_count == 0 ? 1 : allow_count, sizeof(char *));
    bool ok = crawler->allow != NULL;
    if (ok && exclude != NULL)
        ok = (crawler->exclude = strdup(exclude)) != NULL;
    for (; ok && crawler->allow_count < allow_count; crawler->allow_count++)
        ok = (crawler->allow[crawler->allow_count] = strdup(allow[crawler->allow_count])) != NULL;
    if (!ok) {
        crawl_destroy(crawler);
        return TL_NOMEM;
    }
    *out = crawler;
    return TL_OK;
}
void crawl_destroy(tl_crawl *crawler) {
    if (crawler == NULL)
        return;
    for (size_t i = 0; i < crawler->allow_count; i++)
        free(crawler->allow[i]);
    free(crawler->allow);
    free(crawler->exclude);
    free(crawler);
}
static bool ignored_name(const char *name) {
    return name[0] == '.' || strcmp(name, "node_modules") == 0 || strcmp(name, "target") == 0 ||
           strcmp(name, "build") == 0 || strcmp(name, "__pycache__") == 0;
}
static bool allowed_exactly(const tl_crawl *crawler, const char *path) {
    for (size_t i = 0; i < crawler->allow_count; i++) {
        if (strcmp(crawler->allow[i], path) == 0)
            return true;
    }
    return false;
}
/* Whether an allowlisted directory lies strictly below path. */
static bool leads_to_allowed(const tl_crawl *crawler, const char *path) {
    for (size_t i = 0; i < crawler->allow_count; i++) {
        if (strcmp(crawler->allow[i], path) != 0 && path_within(crawler->allow[i], path))
            return true;
    }
    return false;
}
/* The single scope decision shared by walks and crawl_covers. Below a
 * traversed directory only paths leading to allowlisted directories go on;
 * elsewhere hidden/ignored directories are skipped unless allowlisted or on
 * the way to one. Allowlisted directories apply the defaults below them. */
static enum scope classify(const tl_crawl *crawler, const char *path, const char *name, bool is_dir,
                           bool parent_transit) {
    if (path_within(path, crawler->exclude))
        return SCOPE_SKIP;
    if (is_dir && allowed_exactly(crawler, path))
        return SCOPE_INDEX;
    bool leads = is_dir && leads_to_allowed(crawler, path);
    if (parent_transit)
        return leads ? SCOPE_TRANSIT : SCOPE_SKIP;
    if (is_dir && ignored_name(name))
        return leads ? SCOPE_TRANSIT : SCOPE_SKIP;
    return SCOPE_INDEX;
}
static bool failed(const FTSENT *entry) {
    return entry->fts_info == FTS_DNR || entry->fts_info == FTS_ERR || entry->fts_info == FTS_NS;
}
static bool directory(const FTSENT *entry) {
    return entry->fts_info == FTS_D || entry->fts_info == FTS_DNR || entry->fts_info == FTS_DC;
}
static void file_identity(const FTSENT *entry, tl_crawl_entry *record) {
    const struct stat *info = entry->fts_statp;
    record->has_identity = true;
    record->device = (uint64_t)info->st_dev;
    record->inode = (uint64_t)info->st_ino;
    record->identity_sec = (int64_t)info->st_ctim.tv_sec;
    record->identity_nsec = (uint32_t)info->st_ctim.tv_nsec;
    struct statx identity;
    int code = statx(AT_FDCWD, entry->fts_path, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
                     STATX_BASIC_STATS | STATX_BTIME, &identity);
    if (code != 0) {
        if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP) {
            record->has_stat = false;
            record->has_identity = false;
            record->unreadable = true;
        }
        return;
    }
    /* Use one fresh statx observation for metadata and identity, rather than
     * combining an earlier fts stat with a replacement's birth time. */
    if ((identity.stx_mask & STATX_BASIC_STATS) != STATX_BASIC_STATS ||
        identity.stx_size > INT64_MAX) {
        record->has_stat = false;
        record->has_identity = false;
        record->unreadable = true;
        return;
    }
    record->device = (uint64_t)makedev(identity.stx_dev_major, identity.stx_dev_minor);
    record->inode = identity.stx_ino;
    record->is_dir = S_ISDIR(identity.stx_mode);
    record->mtime = identity.stx_mtime.tv_sec;
    record->size = (int64_t)identity.stx_size;
    record->identity_birth = (identity.stx_mask & STATX_BTIME) != 0;
    struct statx_timestamp stamp = record->identity_birth ? identity.stx_btime : identity.stx_ctime;
    record->identity_sec = stamp.tv_sec;
    record->identity_nsec = stamp.tv_nsec;
}
/* The walk's start has no stat'd parent to compare with, so it counts too. */
static bool device_boundary(const FTSENT *entry) {
    if (!directory(entry))
        return false;
    if (entry->fts_level == 0)
        return true;
    const FTSENT *parent = entry->fts_parent;
    return parent->fts_statp != NULL && entry->fts_statp->st_dev != parent->fts_statp->st_dev;
}
static tl_status report(const FTSENT *entry, bool has_stat, enum walk_mode mode,
                        tl_crawl_callback callback, void *context) {
    if (has_stat && entry->fts_statp == NULL)
        return TL_IO;
    tl_crawl_entry record = {.path = entry->fts_path,
                             .is_dir = directory(entry),
                             .is_root = mode == WALK_ROOT && entry->fts_level == 0,
                             .unreadable = failed(entry),
                             .has_stat = has_stat,
                             .mtime = has_stat ? (int64_t)entry->fts_statp->st_mtime : 0,
                             .size = has_stat ? (int64_t)entry->fts_statp->st_size : 0,
                             .device_boundary = has_stat && device_boundary(entry)};
    if (has_stat)
        file_identity(entry, &record);
    return callback(context, &record);
}
static tl_status visit(tl_crawl *crawler, FTS *walk, FTSENT *entry, enum walk_mode mode,
                       tl_crawl_callback callback, void *context) {
    if (entry->fts_info == FTS_DP)
        return TL_OK;
    /* A children-only walk reports level-1 directories but never enters them. */
    if (mode == WALK_CHILDREN && entry->fts_level == 1 && entry->fts_info == FTS_D &&
        fts_set(walk, entry, FTS_SKIP) != 0)
        return TL_IO;
    /* The root is always walked (explicit roots override name defaults), but
     * the excluded scope wins even there. */
    enum scope scope =
        entry->fts_level == 0
            ? (path_within(entry->fts_path, crawler->exclude) ? SCOPE_SKIP : SCOPE_INDEX)
            : classify(crawler, entry->fts_path, entry->fts_name, directory(entry),
                       entry->fts_parent->fts_number == SCOPE_TRANSIT);
    if (scope == SCOPE_SKIP) {
        if (entry->fts_info == FTS_D && fts_set(walk, entry, FTS_SKIP) != 0)
            return TL_IO;
        return TL_OK;
    }
    /* Without a readable root nothing was scanned, so the caller must not prune. */
    if (entry->fts_level == 0 && failed(entry))
        return TL_IO;
    if (scope == SCOPE_TRANSIT) {
        entry->fts_number = SCOPE_TRANSIT;
        /* Not indexed itself; an unreadable one still protects saved entries. */
        return failed(entry) ? report(entry, false, mode, callback, context) : TL_OK;
    }
    /* An unreadable directory still has stat data; FTS_NS/FTS_ERR have none. A
     * directory cycle (only possible through bind mounts) is reported but fts
     * does not descend into it again. */
    return report(entry, entry->fts_info != FTS_NS && entry->fts_info != FTS_ERR, mode, callback,
                  context);
}
static tl_status walk_from(tl_crawl *crawler, const char *root, enum walk_mode mode,
                           tl_crawl_callback callback, void *context) {
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
        status = visit(crawler, walk, entry, mode, callback, context);
        if (status != TL_OK)
            break;
    }
    if (fts_close(walk) != 0 && status == TL_OK)
        status = TL_IO;
    free(canonical);
    return status;
}
tl_status crawl_run(tl_crawl *crawler, const char *root, tl_crawl_callback callback,
                    void *context) {
    return walk_from(crawler, root, WALK_ROOT, callback, context);
}
tl_status crawl_scope(tl_crawl *crawler, const char *directory, bool recursive,
                      tl_crawl_callback callback, void *context) {
    if (directory == NULL || directory[0] != '/')
        return TL_INVALID;
    return walk_from(crawler, directory, recursive ? WALK_TREE : WALK_CHILDREN, callback, context);
}
bool crawl_covers(const tl_crawl *crawler, const char *outer, const char *inner) {
    if (crawler == NULL || outer == NULL || inner == NULL || !path_within(inner, outer) ||
        path_within(outer, crawler->exclude))
        return false;
    size_t length = strlen(inner);
    char *path = strdup(inner);
    if (path == NULL)
        return false;
    enum scope scope = SCOPE_INDEX;
    /* Classify each directory from just below outer down to inner. */
    for (size_t end = strlen(outer); end < length && scope != SCOPE_SKIP;) {
        size_t start = end + (inner[end] == '/' ? 1 : 0);
        end = start;
        while (end < length && inner[end] != '/')
            end++;
        if (end == start)
            continue;
        path[end] = 0;
        scope = classify(crawler, path, path + start, true, scope == SCOPE_TRANSIT);
        path[end] = inner[end];
    }
    free(path);
    return scope == SCOPE_INDEX;
}
void crawl_select_roots(const tl_crawl *crawler, const char *const *roots, size_t count,
                        bool *keep) {
    if (crawler == NULL || roots == NULL || keep == NULL)
        return;
    for (size_t i = 0; i < count; i++) {
        keep[i] = true;
        for (size_t j = 0; j < count && keep[i]; j++) {
            if (i == j)
                continue;
            bool repeat = strcmp(roots[i], roots[j]) == 0;
            /* Of identical roots keep the first; otherwise drop covered ones. */
            keep[i] = repeat ? i < j : !crawl_covers(crawler, roots[j], roots[i]);
        }
    }
}
