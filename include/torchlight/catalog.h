/* Resident immutable lexical snapshots, bounded reader leases and publication. */
#ifndef TORCHLIGHT_CATALOG_H
#define TORCHLIGHT_CATALOG_H
#include "torchlight/lexical.h"

#define CATALOG_MAX_READERS 64
#define CATALOG_MAX_SNAPSHOTS 64
typedef struct tl_catalog tl_catalog;
typedef struct tl_catalog_snapshot tl_catalog_snapshot;
typedef struct tl_catalog_reader tl_catalog_reader;
typedef struct {
    uint64_t catalog_gen;
    size_t entries, snapshots, readers;
    bool available;
} tl_catalog_stats;

/** Create an owned registry with capacity 1..CATALOG_MAX_SNAPSHOTS for active
 * and retired snapshots together. Initially unavailable. TL_INVALID/NOMEM/IO;
 * out NULL on failure. Uses the platform pthread mutex, no external dependency. */
tl_status catalog_create(size_t snapshot_capacity, tl_catalog **out);
/** Destroy registry and owned snapshots; NULL is OK. Caller must stop all other
 * operations first. TL_STATE with no changes if a reader is still leased.
 * Otherwise TL_OK; registry pointer is invalid after success. */
tl_status catalog_destroy(tl_catalog *catalog);
/** Prepare an owned snapshot of sealed *engine with 1..CATALOG_MAX_READERS
 * preallocated workspaces. catalog_gen identifies its committed SQLite view.
 * Transfers *engine and sets it NULL only on success; out NULL on failure.
 * TL_INVALID/STATE/NOMEM/LIMIT/IO. Build on background thread before publication. */
tl_status catalog_snapshot_create(tl_lexical **engine, uint64_t catalog_gen, size_t reader_capacity,
                                  tl_catalog_snapshot **out);
/** Destroy an unpublished snapshot; NULL allowed, no errors. Published snapshots
 * belong to their registry and must never be destroyed by their former owner. */
void catalog_snapshot_destroy(tl_catalog_snapshot *snapshot);
/** Publish prepared *snapshot by a short lifecycle lock; transfer ownership and
 * set it NULL on success. Retire old active view without freeing it. TL_INVALID
 * for NULL; TL_STATE for non-increasing catalog_gen, TL_LIMIT when full.
 * Failures leave active view and caller ownership unchanged. Thread-safe. */
tl_status catalog_publish(tl_catalog *catalog, tl_catalog_snapshot **snapshot);
/** Lease an exclusive preallocated workspace and pin the active snapshot.
 * Thread-safe, no heap allocation or I/O. TL_STATE before first publication,
 * TL_LIMIT when all active workspaces are leased, TL_INVALID for NULL.
 * out NULL on error. Registry must outlive lease; release exactly once. */
tl_status catalog_acquire(tl_catalog *catalog, tl_catalog_reader **out);
/** Release lease without freeing its snapshot; NULL is OK. Thread-safe between
 * different leases, but caller must finish all access to this lease first.
 * Borrowed paths become invalid immediately. No errors, allocation or I/O. */
void catalog_release(tl_catalog_reader *reader);
/** Attach an optional cancellation flag to a lease's workspace; see
 * lexical_workspace_cancel. Ignored for NULL/unleased readers. Detach with
 * NULL before the flag's lifetime ends. No allocation/I/O/errors. */
void catalog_reader_cancel(tl_catalog_reader *reader, const atomic_bool *flag);
/** Query a leased snapshot. Same inputs/errors as lexical_query; out_count zero
 * on error. Paths borrow the lease, including across publication. One thread
 * per lease; separate leases can query concurrently without the lifecycle lock. */
tl_status catalog_query(tl_catalog_reader *reader, const char *query, tl_result *results,
                        size_t capacity, size_t *out_count);
/** Resolve nonzero id in this leased view (acquire current view before launch).
 * Borrow exact raw path until release; out NULL on error. TL_STATE when absent,
 * TL_INVALID for NULL/zero/unleased reader. No allocation or I/O. */
tl_status catalog_resolve(const tl_catalog_reader *reader, uint64_t id, const char **out);
/** Borrow catalog_gen of a lease; zero for NULL (also valid for empty catalog).
 * Caller must hold the lease. No errors. */
uint64_t catalog_reader_gen(const tl_catalog_reader *reader);
/** Detach unleased retired snapshots under the lock and destroy outside it.
 * Call from background writer; no reader thread reclaims. Thread-safe, no
 * allocation. NULL allowed, no errors. */
void catalog_reclaim(tl_catalog *catalog);
/** Copy active view counts and total retained snapshots/leases under the lock.
 * Thread-safe; TL_INVALID for NULL; TL_OK otherwise. No allocation or I/O. */
tl_status catalog_stats(tl_catalog *catalog, tl_catalog_stats *out);
/** Read resident directory metadata while leased; false if absent/unleased.
 * No allocation/I/O; only valid until release. */
bool catalog_is_dir(const tl_catalog_reader *reader, uint64_t id);
/** Pin active metadata without reserving a lexical workspace. Thread-safe,
 * allocation-free; TL_STATE if unavailable, TL_INVALID for NULL. Release once
 * with catalog_unpin; registry must outlive pin. Intended for background copies. */
tl_status catalog_pin(tl_catalog *catalog, tl_catalog_snapshot **out);
/** Release metadata pin; NULL allowed, no allocation/I/O or errors. */
void catalog_unpin(tl_catalog_snapshot *snapshot);
/** Borrow generation/count of a metadata pin, zero for NULL. No errors. */
uint64_t catalog_snapshot_gen(const tl_catalog_snapshot *snapshot);
size_t catalog_snapshot_count(const tl_catalog_snapshot *snapshot);
/** Borrow sorted-id entry at position while pinned; TL_INVALID out-of-range or
 * NULL. Outputs borrow pin. No allocation/I/O. */
tl_status catalog_snapshot_entry(const tl_catalog_snapshot *snapshot, size_t position, uint64_t *id,
                                 const char **path, bool *is_dir);
/** Borrow scoped raw path context under a metadata pin; see lexical_context_path.
 * NULL for invalid input; no allocation/I/O/errors. */
const char *catalog_snapshot_context(const tl_catalog_snapshot *snapshot, size_t position);
#endif
