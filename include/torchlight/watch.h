/* Bounded Linux inotify watches, rename pairing and reconciliation signals. */
#ifndef TORCHLIGHT_WATCH_H
#define TORCHLIGHT_WATCH_H
#include "torchlight/common.h"
#include <stdbool.h>
typedef struct tl_watch tl_watch;
/* path is the affected entry (or the watched directory itself for its own
 * removal), old_path the paired source of a rename. created marks an entry
 * that appeared (created, or moved in without a paired source), whose subtree
 * nobody has seen yet. metadata marks a change that left the entry's name and
 * place alone: attributes (permissions, times, link count) or, when fed,
 * contents written and closed. */
typedef struct {
    const char *path, *old_path;
    bool is_dir, overflow, created, metadata;
} tl_watch_event;
typedef tl_status (*tl_watch_callback)(void *context, const tl_watch_event *event);
/* unavailable counts failed watch_add calls since creation or the last
 * watch_begin, so it describes the current coverage, not all time. */
typedef struct {
    size_t directories;
    uint64_t overflows, unavailable;
} tl_watch_stats;
/* The filesystem object a crawl saw at a directory's path. */
typedef struct {
    uint64_t device, inode;
} tl_watch_identity;
/** Decide whether watch_sweep keeps an unconfirmed directory (borrowed path),
 * e.g. one below a root this scan could not read. */
typedef bool (*tl_watch_keep)(void *context, const char *path);
/** Create owned nonblocking watcher for at most capacity (1..1000000) watched
 * directories at once; slots of watches the kernel dropped (deleted or
 * unmounted directories) are reused. No other thread may use it
 * simultaneously. out NULL on TL_INVALID/NOMEM/IO. */
tl_status watch_create(size_t capacity, tl_watch **out);
/** Close watches and free paths/pending cookies; NULL allowed, no errors. */
void watch_destroy(tl_watch *watch);
/** Install a directory watch during crawling, copying path. identity
 * (optional, borrowed) is the directory the crawl saw there: when the watcher
 * already watches that same object under path, the call only confirms it
 * for this scan generation and makes no system call. Otherwise
 * inotify_add_watch runs; for an object already watched under another path
 * (moved, events not drained yet) the stored path is updated. TL_LIMIT when
 * capacity directories are already watched, TL_IO for unavailable/kernel
 * limit, TL_NOMEM/INVALID; a failed watch is removed again. Failures
 * increment unavailable; the caller must rescan that directory periodically.
 * Subscribes to entries created, deleted and moved, attribute changes and the
 * directory's own removal or move; writes are not subscribed, since they
 * never change a name. */
tl_status watch_add(tl_watch *watch, const char *path, const tl_watch_identity *identity);
/** Start a scan generation (a full scan): later watch_add calls and directory
 * moves confirm watches, and watch_sweep removes the ones nothing confirmed.
 * Resets the unavailable counter. NULL allowed; no errors. */
void watch_begin(tl_watch *watch);
/** After a complete scan: remove every watch not confirmed since watch_begin
 * (directories deleted, now excluded or no longer under a root), except
 * those keep (optional) spares. Returns how many were removed; 0 for NULL.
 * Their pending IN_IGNORED events are discarded silently. No allocation. */
size_t watch_sweep(tl_watch *watch, tl_watch_keep keep, void *context);
/** Drain at most 256 KiB of kernel events; paths borrow callback lifetime.
 * Paired cookies provide old_path; unmatched moves still cause reconciliation.
 * TL_INVALID/IO/NOMEM plus callback errors. Pending cookie storage is bounded;
 * cookie exhaustion emits an overflow signal, never silently loses updates. */
tl_status watch_drain(tl_watch *watch, tl_watch_callback callback, void *context);
/** Feed kernel-format events (also used for deterministic overflow tests).
 * Same lifetime/errors as drain, malformed buffers return TL_INVALID. */
tl_status watch_feed(tl_watch *watch, const void *bytes, size_t length, tl_watch_callback callback,
                     void *context);
/** Drop unmatched cookies after reconciliation. NULL allowed, no errors. */
void watch_clear_moves(tl_watch *watch);
/** Borrow descriptor for poll, -1 for NULL; no ownership transfer or errors. */
int watch_descriptor(const tl_watch *watch);
/** Copy counters, or zero counts for NULL; no ownership/errors. */
tl_watch_stats watch_stats(const tl_watch *watch);
/** Whether inotify reports every change on a filesystem of this statfs f_type.
 * Network and userspace filesystems (NFS, SMB/CIFS, FUSE, 9p, Ceph, AFS, Coda)
 * report only changes made through this machine's kernel, so changes by other
 * clients or the server go unseen. Pure; no errors. */
bool watch_type_reliable(uint64_t filesystem_type);
/** statfs the filesystem holding path (borrowed) and report through *out
 * whether inotify covers it (watch_type_reliable). TL_INVALID for NULL
 * arguments, TL_IO when statfs fails (*out unchanged). No allocation. */
tl_status watch_reliable(const char *path, bool *out);
#endif
