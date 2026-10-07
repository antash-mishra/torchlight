/* Bounded background model/cache publication and asynchronous hybrid queries. */
#ifndef TORCHLIGHT_SEMANTIC_H
#define TORCHLIGHT_SEMANTIC_H
#include "torchlight/catalog.h"
#include "torchlight/desktop.h"
#include "torchlight/ipc.h"
#define SEMANTIC_CLIENTS 16
#define SEMANTIC_DEADLINE_MS 200
#define SEMANTIC_VECTOR_BYTES (150U * 1024U * 1024U)
/* Leave room for the coordinator's current indexing/history status. */
#define SEMANTIC_STATUS_BYTES 1024U
#define SEMANTIC_RESPONSE_BYTES (IPC_RESPONSE_BYTES - SEMANTIC_STATUS_BYTES)
typedef struct tl_semantic tl_semantic;
/* reused counts the rows of the current or last stage that were not embedded
 * (vectors copied from, or rows shared with, the previous snapshot).
 * derived_stages and full_stages count publications since creation: a derived
 * snapshot shares a full base's rows and holds only changed rows. entries and
 * vector_bytes cover both segments of the published snapshot. */
typedef struct {
    bool available, building;
    uint64_t emb_gen, catalog_gen, desktop_gen;
    size_t entries, vector_bytes, processed, total, reused;
    size_t derived_stages, full_stages;
    tl_status last_error;
} tl_semantic_stats;
typedef struct {
    const char *model_path, *database;
    tl_catalog *catalog;
    tl_desktop *desktop;
    size_t vector_budget, metadata_budget;
    unsigned deadline_ms;
} tl_semantic_options;
/** Start owned background service. All strings copied; catalogs must outlive it.
 * Bad/unavailable models fall back to lexical while worker retries. No new
 * inference dependency. TL_INVALID/NOMEM/IO; out NULL on failure. */
tl_status semantic_create(const tl_semantic_options *options, tl_semantic **out);
/** Join worker and free jobs/models/snapshots; NULL allowed. Stop submitting
 * first. No errors. Outstanding queries are discarded during shutdown. */
void semantic_destroy(tl_semantic *semantic);
/** Queue one job per client slot and pin an immutable copied file/desktop/model
 * snapshot matching both supplied generations. All inputs copied or resolved
 * into snapshot-owned metadata; no allocation/SQL/filesystem I/O. out_emb_gen
 * zero on failure. TL_STATE when not ready/mismatched, TL_LIMIT when slot busy,
 * TL_INVALID for inputs. Token is nonzero and prevents connection reuse leaks.
 * Supply fixed lexical candidate pool independently of displayed request.limit. */
tl_status semantic_submit(tl_semantic *semantic, size_t slot, uint64_t token,
                          const tl_ipc_request *request, const char *search_id,
                          uint64_t catalog_gen, uint64_t desktop_gen, const tl_result *lexical,
                          size_t count, uint64_t *out_emb_gen);
/** Copy a ready terminal JSON response, or an immediate lexical terminal on
 * cancel/deadline, into caller buffer. out_length zero while pending (TL_OK).
 * Cancel emits cancelled/superseded. A consumed/unknown token returns TL_STATE;
 * TL_INVALID/LIMIT for arguments/buffer. TL_LIMIT leaves the job available for
 * retry or cancellation; out_length stays zero. Frames fit within
 * SEMANTIC_RESPONSE_BYTES, leaving SEMANTIC_STATUS_BYTES for coordinator status.
 * No allocation/SQL/I/O. In-flight work retains its own snapshot until completion
 * even after cancellation. */
tl_status semantic_take(tl_semantic *semantic, size_t slot, uint64_t token, bool cancel,
                        char *output, size_t capacity, size_t *out_length);
/** Borrow eventfd used to wake coordinator; -1 for NULL. Caller must not close
 * it. No ownership transfer/errors. Drain via semantic_drain after POLLIN. */
int semantic_descriptor(const tl_semantic *semantic);
/** Drain nonblocking notification counter; NULL allowed, no errors. */
void semantic_drain(tl_semantic *semantic);
/** Copy resident/progress statistics under a short lock; TL_INVALID for NULL,
 * TL_OK otherwise. No allocation/SQL/filesystem I/O. Availability describes
 * the published view; query submission separately validates source versions. */
tl_status semantic_stats(tl_semantic *semantic, tl_semantic_stats *out);
#endif
