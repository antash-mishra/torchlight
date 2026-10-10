/* Background catalog reconciliation/publication and bounded asynchronous history,
 * split between an indexing thread and a persistence thread. */
#ifndef TORCHLIGHT_WRITER_H
#define TORCHLIGHT_WRITER_H
#include "torchlight/catalog.h"
#include "torchlight/config.h"
#include "torchlight/ipc.h"
#include "torchlight/usage.h"
#include "torchlight/watch.h"
typedef struct tl_writer tl_writer;
#define WRITER_HISTORY_CAPACITY 256
#define WRITER_RENAME_CAPACITY 256
/* Why a pass scanned every root (see tl_writer_stats.last_full_reason). */
typedef enum {
    WRITER_FULL_NONE,      /* no full pass has completed yet */
    WRITER_FULL_STARTUP,   /* the first pass after start */
    WRITER_FULL_RECONCILE, /* an explicit writer_reconcile request */
    WRITER_FULL_OVERFLOW,  /* inotify lost events or could not be drained */
    WRITER_FULL_SCOPES,    /* more scopes than one pass holds, or a root itself changed */
    WRITER_FULL_FAILURE,   /* retry after a failed pass */
    WRITER_FULL_REPAIR,    /* coverage needs it: no watcher, a root came back, too many repairs */
    WRITER_FULL_MOUNT,     /* the mount table changed */
    WRITER_FULL_BACKSTOP   /* repair_ms passed since the last full pass */
} tl_writer_full_reason;
typedef struct {
    const tl_config *config;
    tl_catalog *catalog;
    const char *socket_path;
    size_t watch_capacity, max_entries, max_path_bytes, readers;
    /* rescan_ms (at least 100): how often the repair set (directories inotify
     * cannot keep current) is rescanned. repair_ms: the backstop interval of a
     * full scan of every root; 0 disables it. */
    unsigned rescan_ms, repair_ms, history_days;
    bool history;
    /* Optional publication adapter for deterministic fault-injection tests. */
    tl_status (*publish)(void *context, tl_catalog *catalog, tl_catalog_snapshot **snapshot);
    void *publish_context;
    /* Optional event-source adapter for deterministic overflow replay tests. */
    tl_status (*drain)(void *context, tl_watch *watch, tl_watch_callback callback,
                       void *callback_context);
    void *drain_context;
    /* Optional watcher factory for deterministic instance-exhaustion tests.
     * Same ownership/errors as watch_create; TL_IO permits scan-only fallback. */
    tl_status (*create_watch)(void *context, size_t capacity, tl_watch **out);
    void *watch_context;
    /* Smallest delta bound before a full rebuild compacts it; 0 selects the
     * default. Small values let tests exercise compaction. */
    size_t delta_entries;
    /* Optional mount-table signal for deterministic tests: return an owned
     * descriptor (the writer closes it) that polls POLLPRI or POLLERR after a
     * mount change, or -1 for none. NULL opens /proc/self/mountinfo, whose
     * poll reports each change once. A change schedules a full scan. */
    int (*open_mounts)(void *context);
    void *mounts_context;
    /* Optional: called on the indexing thread, at most once per second, after
     * a full scan's batch or a retired whole engine was freed, so the process
     * can hand freed heap back to the OS. The library assumes no allocator. */
    void (*release_memory)(void *context);
    void *release_context;
} tl_writer_options;
typedef struct {
    bool indexing, degraded, history_enabled, watch_degraded, recovering;
    /* scoped_reconciliations counts passes that rescanned only event scopes;
     * delta_publications counts snapshots published as a delta over the base,
     * full_builds whole-engine rebuilds (startup, recovery, compaction). */
    uint64_t reconciliations, scoped_reconciliations, delta_publications, full_builds,
        watch_overflows, watch_unavailable, history_dropped, history_failures, history_written;
    /* repair_scopes: directories rescanned every rescan_ms because inotify
     * cannot cover them (unwatched, unreadable, network/FUSE mounts). */
    size_t watches, history_pending, offline_roots, unreadable_scopes, repair_scopes;
    uint64_t last_scan_ms;
    tl_writer_full_reason last_full_reason;
} tl_writer_stats;
/** Create owned writer, load/publish the saved catalog before crawling and start
 * its indexing and persistence threads. The persistence thread owns the SQLite
 * write connection and serializes catalog batches, history and retention; the
 * indexing thread crawls into a private batch, builds engines from committed
 * rows on a read connection and publishes. No scan or index build runs inside
 * a write transaction. config and catalog must outlive it; catalog needs
 * capacity two, with the writer as its only publisher. Config accepts root/allow;
 * missing roots are retained and retried. Options are copied, socket path copied.
 * Unavailable inotify instances retain the old watcher if any, continue scans
 * with degraded watch status, and retry watch creation at later reconciliations.
 * Snapshot builds enforce entry/path-byte limits and at most one staged engine.
 * TL_INVALID/NOMEM/IO/STATE/LIMIT; out NULL on error. SQLite owned by worker
 * after startup; caller must hold the daemon's database singleton lock. */
tl_status writer_create(const tl_writer_options *options, tl_writer **out);
/** Stop both threads, roll back interrupted batches and join; accepted history
 * drains before exit within a five-second drain deadline plus any bounded
 * in-progress SQLite busy wait. Catalog remains caller-owned. NULL allowed. */
void writer_destroy(tl_writer *writer);
/** Request reconciliation (one coalesced pending flag). Thread-safe, bounded,
 * no allocation/I/O. TL_INVALID for NULL; TL_OK otherwise. */
tl_status writer_reconcile(tl_writer *writer);
/** Enqueue optional history by copied request/search id; FIFO preserves search
 * before open. QUERY/OPEN/HISTORY_CLEAR only. An OPEN whose search_id and
 * query are both set also saves that search row, in the same transaction.
 * Thread-safe, no allocation or SQL. The persistence thread writes queued
 * events in one transaction per drain, independently of scans and builds.
 * TL_LIMIT on saturation (increments dropped), TL_STATE when history disabled,
 * TL_INVALID for inputs; accepted events may fail asynchronously with counters.
 * If the batch cannot begin (the database is locked elsewhere), that drain
 * writes each event alone without retrying the begin. */
tl_status writer_history(tl_writer *writer, const tl_ipc_request *request, const char *search_id);
/** Take the usage summary the persistence thread built from retained history
 * at startup (before writing any newly queued event). Returns false while that
 * load is still running; then true, once with *out owning the summary (the
 * caller destroys it) and afterwards with *out NULL. *out is also NULL with
 * history disabled or after a failed load (counted in history_failures).
 * Thread-safe; no allocation or I/O. */
bool writer_take_usage(tl_writer *writer, tl_usage **out);
/** Copy coherent worker status under a short mutex. TL_INVALID for NULL;
 * TL_OK otherwise. No allocation/I/O; mutex never held during SQL/crawl/build. */
tl_status writer_stats(tl_writer *writer, tl_writer_stats *out);
/** Static lowercase name of a full-scan reason ("startup", "backstop", ...),
 * "none" for WRITER_FULL_NONE and unknown values. No errors. */
const char *writer_full_reason_name(tl_writer_full_reason reason);
#endif
