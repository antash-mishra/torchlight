/* fts-based physical scans: default exclusions, allowlist traversal, and
 * unreadable scopes reported separately from excluded trees. */
#include "torchlight/crawl.h"
#include <errno.h>
#include <fts.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
struct tl_crawl {
    char *exclude;
    char **allow;
    size_t allow_count;
};
/* What a walk does with a path: report it, pass through it to reach an
 * allowlisted directory below, or leave it (and its subtree) alone. */
enum scope { SCOPE_INDEX, SCOPE_TRANSIT, SCOPE_SKIP };
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
/* Byte-wise: path is scope or below it ("/a/b" is within "/a", "/ab" is not). */
static bool within(const char *path, const char *scope) {
    if (scope == NULL)
        return false;
    size_t length = strlen(scope);
    return strncmp(path, scope, length) == 0 &&
           (path[length] == 0 || path[length] == '/' || (length == 1 && scope[0] == '/'));
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
        if (strcmp(crawler->allow[i], path) != 0 && within(crawler->allow[i], path))
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
    if (within(path, crawler->exclude))
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
static tl_status report(const FTSENT *entry, bool has_stat, tl_crawl_callback callback,
                        void *context) {
    if (has_stat && entry->fts_statp == NULL)
        return TL_IO;
    tl_crawl_entry record = {.path = entry->fts_path,
                             .is_dir = directory(entry),
                             .is_root = entry->fts_level == 0,
                             .unreadable = failed(entry),
                             .has_stat = has_stat,
                             .mtime = has_stat ? (int64_t)entry->fts_statp->st_mtime : 0,
                             .size = has_stat ? (int64_t)entry->fts_statp->st_size : 0};
    return callback(context, &record);
}
static tl_status visit(tl_crawl *crawler, FTS *walk, FTSENT *entry, tl_crawl_callback callback,
                       void *context) {
    if (entry->fts_info == FTS_DP)
        return TL_OK;
    /* The root is always walked (explicit roots override name defaults), but
     * the excluded scope wins even there. */
    enum scope scope = entry->fts_level == 0
                           ? (within(entry->fts_path, crawler->exclude) ? SCOPE_SKIP : SCOPE_INDEX)
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
        return failed(entry) ? report(entry, false, callback, context) : TL_OK;
    }
    /* An unreadable directory still has stat data; FTS_NS/FTS_ERR have none. A
     * directory cycle (only possible through bind mounts) is reported but fts
     * does not descend into it again. */
    return report(entry, entry->fts_info != FTS_NS && entry->fts_info != FTS_ERR, callback,
                  context);
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
bool crawl_covers(const tl_crawl *crawler, const char *outer, const char *inner) {
    if (crawler == NULL || outer == NULL || inner == NULL || !within(inner, outer) ||
        within(outer, crawler->exclude))
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
