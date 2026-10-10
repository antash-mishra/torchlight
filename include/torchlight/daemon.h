/* Resident lexical/hybrid daemon orchestration with bounded nonblocking clients. */
#ifndef TORCHLIGHT_DAEMON_H
#define TORCHLIGHT_DAEMON_H
#include "torchlight/writer.h"
#define DAEMON_MAX_CLIENTS 16
#define DAEMON_ACTIVE_REQUESTS 4
typedef struct tl_daemon tl_daemon;
typedef struct {
    const tl_config *config;
    const char *socket_path;
    const char *model_path;
    size_t watch_capacity, max_entries, max_path_bytes;
    /* rescan_ms: repair-set interval; repair_ms: backstop full-scan interval,
     * 0 disables it (see tl_writer_options). */
    unsigned rescan_ms, repair_ms, history_days;
    unsigned semantic_deadline_ms;
    bool history;
    /* Optional hook the writer calls after freeing a full scan's batch or a
     * retired whole engine (see tl_writer_options.release_memory); the
     * executable owns the policy. */
    void (*release_memory)(void *context);
    void *release_context;
} tl_daemon_options;
/** Create owned daemon with socket/database singleton locks, saved resident
 * catalog, writer, preallocated client buffers and a search thread. config must
 * outlive daemon. Blocks SIGINT/SIGTERM on this thread before creating workers;
 * destroy restores its signal mask. Call run/destroy on the creating thread.
 * TL_INVALID/NOMEM/IO/STATE/LIMIT; out NULL on errors. No synchronous crawl. */
tl_status daemon_create(const tl_daemon_options *options, tl_daemon **out);
/** Serve until SIGINT/SIGTERM, then return TL_OK; poll/accept failures TL_IO.
 * One run call at a time, on creating thread. This thread only reads, frames
 * and writes sockets; the search thread serves each client's bounded request
 * queue in order. A newer query supersedes that client's queued queries and
 * cancels a running one; they still answer as cancelled. Query execution
 * performs no SQL, filesystem reads or heap allocation. Slow clients close
 * after five seconds; peers that close outright drop their pending work. */
tl_status daemon_run(tl_daemon *daemon);
/** Join search thread and writer, release sockets/locks/snapshots and restore signal mask.
 * NULL allowed. TL_STATE if an internal catalog lease remains (daemon remains
 * owned by caller for retry). Call after run and all borrowed results finish. */
tl_status daemon_destroy(tl_daemon *daemon);
#endif
