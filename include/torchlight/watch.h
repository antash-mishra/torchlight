/* Bounded Linux inotify watches, rename pairing and reconciliation signals. */
#ifndef TORCHLIGHT_WATCH_H
#define TORCHLIGHT_WATCH_H
#include "torchlight/common.h"
#include <stdbool.h>
typedef struct tl_watch tl_watch;
typedef struct {
    const char *path, *old_path;
    bool is_dir, overflow;
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
 * increment unavailable; caller must reconcile periodically. */
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
#endif
