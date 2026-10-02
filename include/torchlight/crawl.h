/* Physical filesystem walking with explicit roots, default exclusions, an
 * allowlist for hidden/ignored directories and a never-crawled state scope. */
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
    /* Valid only when has_stat; a failed stat reports zeros. Unreadable
     * directories that are only traversed (see crawl_run) have no stat. */
    bool has_stat;
    int64_t mtime, size;
    /* Filesystem incarnation, valid only with has_stat and has_identity.
     * Birth time distinguishes reused inodes; ctime is the conservative
     * fallback when the filesystem/kernel cannot supply birth time. */
    bool has_identity, identity_birth;
    uint64_t device, inode;
    int64_t identity_sec;
    uint32_t identity_nsec;
} tl_crawl_entry;
typedef tl_status (*tl_crawl_callback)(void *context, const tl_crawl_entry *entry);
/** Create a crawler. exclude (optional) is a scope never crawled, not even
 * below allowlisted or root directories (Torchlight's own state). allow lists
 * allow_count directories re-included despite the hidden/ignored-name
 * defaults. All paths should be absolute and canonical; they are copied.
 * out NULL on TL_INVALID/TL_NOMEM. */
tl_status crawl_create(const char *exclude, const char *const *allow, size_t allow_count,
                       tl_crawl **out);
/** Free crawler; NULL allowed, no errors. */
void crawl_destroy(tl_crawl *crawler);
/** Walk canonical root physically, including symlink entries but not targets.
 * Callback borrows entry/path only during call; context is caller-owned.
 *
 * Hidden directories and node_modules/target/build/__pycache__ are skipped;
 * hidden files are retained; the root itself is always walked. An allowlisted
 * directory is indexed like any other; its hidden or ignored ancestors below
 * root are traversed without being reported, following only paths that lead
 * to allowlisted directories. The exclude scope always wins.
 *
 * Below the root, unreadable directories and failed stats do not stop the
 * walk: they are reported with unreadable set, so the caller can keep their
 * saved entries instead of pruning them. A directory is first reported
 * normally and, if listing it then fails, again as unreadable. A missing or
 * unreadable root, or a walk failure, returns TL_IO. Callback failures
 * propagate. Stat records include device/inode and birth time where available,
 * otherwise ctime; symlink identity belongs to the link itself. No deletion is
 * performed. Identity/stat lookup failures keep the scope unreadable. */
tl_status crawl_run(tl_crawl *crawler, const char *root, tl_crawl_callback callback, void *context);
/** Return whether crawl_run(outer) would index directory inner itself (both
 * absolute and canonical): inner is outer or below it, and no excluded,
 * skipped or traversed-only directory lies on the way. Then a separate scan of
 * inner is redundant. false for NULL arguments. */
bool crawl_covers(const tl_crawl *crawler, const char *outer, const char *inner);
/** Decide which of count canonical roots need their own scan: keep[i] is false
 * for repeats of an earlier root and for roots another listed root covers
 * (see crawl_covers). keep must hold count flags. No-op for NULL arguments. */
void crawl_select_roots(const tl_crawl *crawler, const char *const *roots, size_t count,
                        bool *keep);
#endif
