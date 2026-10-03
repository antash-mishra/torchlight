/* Testable popup response/selection model, independent of GTK widgets. */
#ifndef TORCHLIGHT_POPUP_H
#define TORCHLIGHT_POPUP_H
#include "torchlight/ipc.h"
#define POPUP_RESULTS 10
#define POPUP_PATH_BYTES 8192
#define POPUP_TEXT_BYTES 4096
typedef struct tl_popup_model tl_popup_model;
typedef struct {
    uint64_t id, desktop_revision;
    char path[POPUP_PATH_BYTES], display[POPUP_PATH_BYTES];
    char name[POPUP_TEXT_BYTES], icon[POPUP_TEXT_BYTES], desktop_id[POPUP_TEXT_BYTES];
    bool application, settings, folder;
} tl_popup_row;
/** Create owned model/scratch. TL_INVALID/NOMEM; out NULL on error. */
tl_status popup_model_create(tl_popup_model **out);
/** Free model; borrowed row pointers expire. NULL allowed. */
void popup_model_destroy(tl_popup_model *model);
/** Set latest request id and invalidate activation/reset deliberate selection.
 * id borrowed/copied (<=IPC_REQUEST_ID_BYTES). TL_INVALID/LIMIT. */
tl_status popup_model_begin(tl_popup_model *model, const char *id);
/** Clear rows, selection, search/status metadata and the current request id.
 * All pending responses become obsolete and borrowed row pointers expire.
 * No errors or ownership transfers; NULL allowed. */
void popup_model_clear(tl_popup_model *model);
/** Decode/apply current response; obsolete id returns TL_STATE without changes.
 * Final reordering preserves deliberately selected id AND position, including
 * one retained row outside top-k. TL_INVALID/LIMIT/STATE/IO; no borrowed input. */
tl_status popup_model_apply(tl_popup_model *model, const char *response, size_t length);
/** Decode validated response into caller rows, copying exact path bytes and
 * single-line labels. Uses model scratch, leaves selection untouched. Returns
 * TL_STATE for stale_result, TL_IO for remote error; TL_INVALID/LIMIT malformed.
 * search_id copied (may be empty); count zero on error. */
tl_status popup_model_decode(tl_popup_model *model, const char *response, size_t length,
                             tl_popup_row *rows, size_t capacity, size_t *count,
                             char search_id[IPC_HISTORY_ID_BYTES + 1]);
/** Move clamped selection, keeping activation disabled until current results.
 * No errors. delta normally +/-1; no ownership changes. */
void popup_model_move(tl_popup_model *model, int delta);
/** Borrow current selected row until begin/apply/destroy, NULL if not actionable. */
const tl_popup_row *popup_model_selected(const tl_popup_model *model);
/** Borrow row at index until next mutation; NULL for absent index. */
const tl_popup_row *popup_model_row(const tl_popup_model *model, size_t index);
/** Read row count/selection/status/search id; borrowed search id expires on apply.
 * NULL-safe getters have zero/false/empty defaults; no errors. */
size_t popup_model_count(const tl_popup_model *model);
size_t popup_model_position(const tl_popup_model *model);
bool popup_model_indexing(const tl_popup_model *model);
bool popup_model_degraded(const tl_popup_model *model);
const char *popup_model_search_id(const tl_popup_model *model);
#endif
