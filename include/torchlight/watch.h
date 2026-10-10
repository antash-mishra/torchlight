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
typedef struct {
    size_t directories;
    uint64_t overflows, unavailable;
} tl_watch_stats;
/** Create owned nonblocking watcher with 1..1000000 directory slots. No other
 * thread may use it simultaneously. out NULL on TL_INVALID/NOMEM/IO. */
tl_status watch_create(size_t capacity, tl_watch **out);
/** Close watches and free paths/pending cookies; NULL allowed, no errors. */
void watch_destroy(tl_watch *watch);
/** Install a directory watch during crawling, copying path. TL_LIMIT for slot
 * exhaustion, TL_IO for unavailable/kernel limit, TL_NOMEM/INVALID. Failures
 * increment unavailable; the caller must rescan that directory periodically.
 * Subscribes to entries created, deleted and moved, attribute changes and the
 * directory's own removal or move; writes are not subscribed, since they
 * never change a name. */
tl_status watch_add(tl_watch *watch, const char *path);
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
