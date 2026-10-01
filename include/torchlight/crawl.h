/* Physical filesystem walking with explicit root and state exclusions. */
#ifndef TORCHLIGHT_CRAWL_H
#define TORCHLIGHT_CRAWL_H
#include "torchlight/common.h"
#include <stdbool.h>
typedef struct tl_crawl tl_crawl;
typedef struct {
    const char *path;
    bool is_dir, is_root;
    /* True when this scope could not be fully read (unreadable directory or
     * failed stat). Its saved catalog entries must be kept, not pruned. */
    bool unreadable;
    /* Valid only when has_stat; a failed stat reports zeros. */
    bool has_stat;
    int64_t mtime, size;
} tl_crawl_entry;
typedef tl_status (*tl_crawl_callback)(void *context, const tl_crawl_entry *entry);
/** Create a crawler, copying optional excluded directory/file. out NULL on
 * invalid/allocation failure. Exclusion should be an absolute canonical path.
 * Roots override hidden/ignored-name defaults, but never this exclusion. */
tl_status crawl_create(const char *exclude, tl_crawl **out);
/** Free crawler; NULL allowed, no errors. */
void crawl_destroy(tl_crawl *crawler);
/** Walk canonical root physically, including symlink entries but not targets.
 * Callback borrows entry/path only during call; context is caller-owned.
 * Hidden directories and node_modules/target/build/__pycache__ are skipped;
 * hidden files are retained. Below the root, unreadable directories and failed
 * stats do not stop the walk: they are reported with unreadable set, so the
 * caller can keep their saved entries instead of pruning them. A directory is
 * first reported normally and, if listing it then fails, again as unreadable.
 * A missing or
 * unreadable root, or a walk failure, returns TL_IO. Callback failures
 * propagate. No deletion is performed. */
tl_status crawl_run(tl_crawl *crawler, const char *root, tl_crawl_callback callback, void *context);
#endif
