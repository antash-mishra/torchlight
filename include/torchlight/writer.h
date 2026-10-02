/* Background catalog reconciliation/publication and bounded asynchronous history. */
#ifndef TORCHLIGHT_WRITER_H
#define TORCHLIGHT_WRITER_H
#include "torchlight/catalog.h"
#include "torchlight/config.h"
#include "torchlight/ipc.h"
#include "torchlight/watch.h"
typedef struct tl_writer tl_writer;
#define WRITER_HISTORY_CAPACITY 256
#define WRITER_RENAME_CAPACITY 256
typedef struct {
    const tl_config *config;
    tl_catalog *catalog;
    const char *socket_path;
    size_t watch_capacity, max_entries, max_path_bytes, readers;
    unsigned rescan_ms, history_days;
    bool history;
    /* Optional publication adapter for deterministic fault-injection tests. */
    tl_status (*publish)(void *context, tl_catalog *catalog, tl_catalog_snapshot **snapshot);
    void *publish_context;
    /* Optional event-source adapter for deterministic overflow replay tests. */
    tl_status (*drain)(void *context, tl_watch *watch, tl_watch_callback callback,
                       void *callback_context);
    void *drain_context;
} tl_writer_options;
typedef struct {
    bool indexing, degraded, history_enabled, watch_degraded, recovering;
    uint64_t reconciliations, watch_overflows, watch_unavailable, history_dropped, history_failures;
    size_t watches, history_pending, offline_roots, unreadable_scopes;
    uint64_t last_scan_ms;
} tl_writer_stats;
/** Create owned writer, load/publish the saved catalog before crawling and start
 * its worker. config and catalog must outlive it; catalog needs capacity two,
 * with the writer as its only publisher. Config accepts root/allow;
 * missing roots are retained and retried. Options are copied, socket path copied.
 * Snapshot builds enforce entry/path-byte limits and at most one staged engine.
 * TL_INVALID/NOMEM/IO/STATE/LIMIT; out NULL on error. SQLite owned by worker
 * after startup; caller must hold the daemon's database singleton lock. */
tl_status writer_create(const tl_writer_options *options, tl_writer **out);
/** Stop worker, roll back interrupted scans and join; accepted history drains
 * before exit within a five-second drain deadline plus any bounded in-progress
 * SQLite busy wait. Catalog remains caller-owned. NULL allowed, no errors. */
void writer_destroy(tl_writer *writer);
/** Request reconciliation (one coalesced pending flag). Thread-safe, bounded,
 * no allocation/I/O. TL_INVALID for NULL; TL_OK otherwise. */
tl_status writer_reconcile(tl_writer *writer);
/** Enqueue optional history by copied request/search id; FIFO preserves search
 * before open. QUERY/OPEN/HISTORY_CLEAR only. Thread-safe, no allocation or SQL.
 * TL_LIMIT on saturation (increments dropped), TL_STATE when history disabled,
 * TL_INVALID for inputs; accepted events may fail asynchronously with counters. */
tl_status writer_history(tl_writer *writer, const tl_ipc_request *request, const char *search_id);
/** Copy coherent worker status under a short mutex. TL_INVALID for NULL;
 * TL_OK otherwise. No allocation/I/O; mutex never held during SQL/crawl/build. */
tl_status writer_stats(tl_writer *writer, tl_writer_stats *out);
#endif
