/* Incremental catalog publication: the entries changed since the published
 * base engine, and the next delta snapshot built from a committed change set.
 * Used by the writer's indexing thread only. */
#ifndef TORCHLIGHT_DELTA_H
#define TORCHLIGHT_DELTA_H
#include "torchlight/catalog.h"
#include "torchlight/store.h"
typedef struct tl_delta tl_delta;
/* Bounds of one delta publication. The delta may hold at most
 * max(min_entries, base entries / divisor) entries before the caller must
 * compact with a full rebuild; whole-snapshot limits match full builds. */
typedef struct {
    size_t max_entries, max_path_bytes, readers, min_entries, divisor;
} tl_delta_limits;
/** Create owned, empty bookkeeping. TL_INVALID for NULL out, TL_NOMEM; out
 * NULL on error. */
tl_status delta_create(tl_delta **out);
/** Free the kept and prepared entries; NULL allowed, no errors. */
void delta_destroy(tl_delta *delta);
/** Record that a full rebuild published a base of entries rows and bytes
 * path bytes at catalog_gen: no entries are kept above it any more. NULL
 * ignored. No allocation/errors. */
void delta_reset(tl_delta *delta, uint64_t catalog_gen, size_t entries, size_t bytes);
/** catalog_gen of the last publication this bookkeeping describes (zero
 * before any); a source snapshot of another generation cannot be extended. */
uint64_t delta_gen(const tl_delta *delta);
/** Build the snapshot following source (pinned, the last publication) for a
 * committed batch that touched ids (ascending, deduplicated): load their rows
 * from store's committed view, merge them with the kept entries, build a
 * delta engine that references source's base, and derive the snapshot that
 * retires ids from the base. *out is owned by the caller (publish it, then
 * delta_commit; on failure delta_abandon). TL_LIMIT when the delta or the
 * snapshot would exceed limits, so the caller compacts with a full rebuild;
 * TL_STATE when source is not the last publication; TL_INVALID/NOMEM/IO plus
 * store and engine errors. Never runs inside a write transaction. */
tl_status delta_prepare(tl_delta *delta, tl_store *store, tl_catalog_snapshot *source,
                        const uint64_t *ids, size_t count, const tl_delta_limits *limits,
                        tl_catalog_snapshot **out);
/** Adopt the entries of the last delta_prepare once its snapshot is
 * published. NULL ignored, no errors. */
void delta_commit(tl_delta *delta);
/** Drop the entries of the last delta_prepare (publication failed). */
void delta_abandon(tl_delta *delta);
#endif
