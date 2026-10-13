/* fts-based physical scans with one stat per entry: default exclusions,
 * excluded scopes, allowlist traversal, and unreadable scopes reported
 * separately from excluded trees. */
#include "torchlight/crawl.h"
#include "torchlight/path.h"
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
/* A directory link count that tells fts nothing about subdirectories. */
#define CRAWL_UNKNOWN_LINKS 1
struct tl_crawl {
    char **exclude, **allow;
    size_t exclude_count, allow_count;
};
/* What a walk does with a path: report it, pass through it to reach an
 * allowlisted directory below, or leave it (and its subtree) alone. */
enum scope { SCOPE_INDEX, SCOPE_TRANSIT, SCOPE_SKIP };
/* A full root walk, a scoped walk of one indexed directory's whole subtree,
 * or of that directory and its direct children only. */
enum walk_mode { WALK_ROOT, WALK_TREE, WALK_CHILDREN };
static bool valid_list(const char *const *paths, size_t count) {
    if (paths == NULL)
        return count == 0;
    for (size_t i = 0; i < count; i++) {
        if (paths[i] == NULL)
            return false;
    }
    return true;
}
/* Copy count strings into a new array at *out; *copied counts the copies so
 * the caller can free a partial list. */
static bool copy_list(const char *const *paths, size_t count, char ***out, size_t *copied) {
    *out = calloc(count == 0 ? 1 : count, sizeof(char *));
    if (*out == NULL)
        return false;
    for (; *copied < count; (*copied)++) {
        if (((*out)[*copied] = strdup(paths[*copied])) == NULL)
            return false;
    }
    return true;
}
tl_status crawl_create(const char *const *exclude, size_t exclude_count, const char *const *allow,
                       size_t allow_count, tl_crawl **out) {
    if (out == NULL)
        return TL_INVALID;
    *out = NULL;
    if (!valid_list(exclude, exclude_count) || !valid_list(allow, allow_count))
        return TL_INVALID;
    tl_crawl *crawler = calloc(1, sizeof(*crawler));
    if (crawler == NULL)
        return TL_NOMEM;
    if (!copy_list(exclude, exclude_count, &crawler->exclude, &crawler->exclude_count) ||
        !copy_list(allow, allow_count, &crawler->allow, &crawler->allow_count)) {
        crawl_destroy(crawler);
        return TL_NOMEM;
    }
    *out = crawler;
    return TL_OK;
}
static void free_list(char **paths, size_t count) {
    for (size_t i = 0; paths != NULL && i < count; i++)
        free(paths[i]);
    free(paths);
}
void crawl_destroy(tl_crawl *crawler) {
    if (crawler == NULL)
        return;
    free_list(crawler->exclude, crawler->exclude_count);
    free_list(crawler->allow, crawler->allow_count);
    free(crawler);
}
/* Whether path is an excluded scope or lies below one. */
static bool excluded(const tl_crawl *crawler, const char *path) {
    for (size_t i = 0; i < crawler->exclude_count; i++) {
        if (path_within(path, crawler->exclude[i]))
            return true;
    }
    return false;
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
    if (excluded(crawler, path))
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
/* A failed lookup cannot prove anything about the entry, so its scope stays
 * unreadable and the caller keeps what was saved there. */
static void lookup_failed(tl_crawl_entry *record) {
    record->has_stat = false;
    record->has_identity = false;
    record->unreadable = true;
}
/* Without statx (old kernels or filesystems): one fstatat, ctime identity. */
static void legacy_identity(const char *path, tl_crawl_entry *record) {
    struct stat info;
    if (fstatat(AT_FDCWD, path, &info, AT_SYMLINK_NOFOLLOW) != 0 || info.st_size < 0) {
        lookup_failed(record);
        return;
    }
    record->has_identity = true;
    record->device = (uint64_t)info.st_dev;
    record->inode = (uint64_t)info.st_ino;
    record->is_dir = S_ISDIR(info.st_mode);
    record->mtime = (int64_t)info.st_mtime;
    record->size = (int64_t)info.st_size;
    record->identity_sec = (int64_t)info.st_ctim.tv_sec;
    record->identity_nsec = (uint32_t)info.st_ctim.tv_nsec;
}
/* The entry's only stat: the walk runs with FTS_NOSTAT, so one statx supplies
 * metadata and identity together, without following symlinks. */
static void file_identity(const char *path, tl_crawl_entry *record) {
    struct statx identity;
    int code = statx(AT_FDCWD, path, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
                     STATX_BASIC_STATS | STATX_BTIME, &identity);
    if (code != 0) {
        if (errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)
            legacy_identity(path, record);
        else
            lookup_failed(record);
        return;
    }
    if ((identity.stx_mask & STATX_BASIC_STATS) != STATX_BASIC_STATS ||
        identity.stx_size > INT64_MAX) {
        lookup_failed(record);
        return;
    }
    record->has_identity = true;
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
/* The walk's start has no parent to compare with, so it counts too. fts keeps
 * fts_dev for every directory it stats, even under FTS_NOSTAT (where
 * fts_statp is NULL for every entry). */
static bool device_boundary(const FTSENT *entry) {
    if (!directory(entry))
        return false;
    return entry->fts_level == 0 || entry->fts_dev != entry->fts_parent->fts_dev;
}
static tl_status report(const FTSENT *entry, bool has_stat, enum walk_mode mode,
                        tl_crawl_callback callback, void *context) {
    tl_crawl_entry record = {.path = entry->fts_path,
                             .is_dir = directory(entry),
                             .is_root = mode == WALK_ROOT && entry->fts_level == 0,
                             .unreadable = failed(entry),
                             .has_stat = has_stat,
                             .device_boundary = has_stat && device_boundary(entry)};
    if (has_stat)
        file_identity(entry->fts_path, &record);
    /* fts skipped the stat because d_type said "not a directory". One that is
     * a directory after all (replaced since the listing) was not entered:
     * keep its saved subtree and let repair rescan it. */
    if (entry->fts_info == FTS_NSOK && record.is_dir && mode != WALK_CHILDREN)
        record.unreadable = true;
    return callback(context, &record);
}
static tl_status visit(tl_crawl *crawler, FTS *walk, FTSENT *entry, enum walk_mode mode,
                       tl_crawl_callback callback, void *context) {
    if (entry->fts_info == FTS_DP)
        return TL_OK;
    /* Under FTS_NOSTAT glibc's fts assumes a directory with link count n holds
     * n - 2 subdirectories and reports every later entry unstat'd, as a file,
     * never entering it. Filesystems that misreport link counts (some FUSE
     * filesystems give every directory 2) would lose whole subtrees, as with
     * find without -noleaf. Count 1 turns that guess off before fts lists the
     * directory; only entries whose d_type says "not a directory" skip it. */
    if (entry->fts_info == FTS_D)
        entry->fts_nlink = CRAWL_UNKNOWN_LINKS;
    /* A children-only walk reports level-1 directories but never enters them. */
    if (mode == WALK_CHILDREN && entry->fts_level == 1 && entry->fts_info == FTS_D &&
        fts_set(walk, entry, FTS_SKIP) != 0)
        return TL_IO;
    /* The root is always walked (explicit roots override name defaults), but
     * excluded scopes win even there. */
    enum scope scope = entry->fts_level == 0
                           ? (excluded(crawler, entry->fts_path) ? SCOPE_SKIP : SCOPE_INDEX)
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
    /* NOCHDIR keeps callbacks and concurrent callers independent of cwd.
     * NOSTAT leaves non-directories to the one statx in file_identity. */
    char *roots[] = {canonical, NULL};
    FTS *walk = fts_open(roots, FTS_PHYSICAL | FTS_NOCHDIR | FTS_NOSTAT, NULL);
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
        excluded(crawler, outer))
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
