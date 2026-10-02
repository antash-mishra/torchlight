/* Resident lexical-only daemon orchestration with bounded nonblocking clients. */
#ifndef TORCHLIGHT_DAEMON_H
#define TORCHLIGHT_DAEMON_H
#include "torchlight/writer.h"
#define DAEMON_MAX_CLIENTS 16
#define DAEMON_ACTIVE_REQUESTS 4
typedef struct tl_daemon tl_daemon;
typedef struct {
    const tl_config *config;
    const char *socket_path;
    size_t watch_capacity, max_entries, max_path_bytes;
    unsigned rescan_ms, history_days;
    bool history;
} tl_daemon_options;
/** Create owned daemon with socket/database singleton locks, saved resident
 * catalog, writer and preallocated client buffers. config must outlive daemon.
 * Blocks SIGINT/SIGTERM on this thread before creating workers; destroy restores
 * its signal mask. Call run/destroy on the creating thread. TL_INVALID/NOMEM/
 * IO/STATE/LIMIT; out NULL on errors. No synchronous filesystem crawl. */
tl_status daemon_create(const tl_daemon_options *options, tl_daemon **out);
/** Serve until SIGINT/SIGTERM, then return TL_OK; poll/accept failures TL_IO.
 * One run call at a time, on creating thread. Query execution performs no SQL,
 * filesystem reads or heap allocation. Slow clients close after five seconds. */
tl_status daemon_run(tl_daemon *daemon);
/** Join writer, release sockets/locks/snapshots and restore signal mask.
 * NULL allowed. TL_STATE if an internal catalog lease remains (daemon remains
 * owned by caller for retry). Call after run and all borrowed results finish. */
tl_status daemon_destroy(tl_daemon *daemon);
#endif
