/* Testable popup response/selection model, independent of GTK widgets. */
#ifndef TORCHLIGHT_POPUP_H
#define TORCHLIGHT_POPUP_H
#include "torchlight/ipc.h"
#include "torchlight/windows.h"
#define POPUP_RESULTS 10
#define POPUP_PATH_BYTES 8192
#define POPUP_TEXT_BYTES 4096
/* Open windows listed under an application before a "more windows" item; few
 * enough that several running applications still leave room for files. */
#define POPUP_WINDOWS_SHOWN 3
/* Every result (plus one retained row), each window once and two other children per result. */
#define POPUP_ITEMS (3 * (POPUP_RESULTS + 1) + WINDOWS_MAX)
typedef struct tl_popup_model tl_popup_model;
typedef struct {
    uint64_t id, desktop_revision;
    char path[POPUP_PATH_BYTES], display[POPUP_PATH_BYTES];
    char name[POPUP_TEXT_BYTES], icon[POPUP_TEXT_BYTES], desktop_id[POPUP_TEXT_BYTES];
    char wm_class[WINDOWS_CLASS_BYTES]; /* application StartupWMClass, may be empty */
    bool application, settings, folder;
} tl_popup_row;
/** What a list item shows: a result, or a child listed beneath an application. */
typedef enum {
    POPUP_ITEM_RESULT,
    POPUP_ITEM_WINDOW,       /* one open window of the parent application */
    POPUP_ITEM_MORE_WINDOWS, /* reveals the parent's remaining windows */
    POPUP_ITEM_NEW_WINDOW    /* opens another window of the parent application */
} tl_popup_item_kind;
typedef struct {
    tl_popup_item_kind kind;
    size_t row;      /* result row index; the parent's for children */
    size_t window;   /* snapshot index: the WINDOW's, or a RESULT's most recent window */
    size_t windows;  /* RESULT: its open windows; MORE_WINDOWS: those still hidden */
    uint64_t handle; /* WINDOW: window-system id; zero otherwise */
    bool expanded;   /* RESULT: its children follow it */
} tl_popup_item;
/** Create owned model/scratch. TL_INVALID/NOMEM; out NULL on error. */
tl_status popup_model_create(tl_popup_model **out);
/** Free model; borrowed row pointers expire. NULL allowed. */
void popup_model_destroy(tl_popup_model *model);
/** Set latest request id, invalidate activation, and reset deliberate selection
 * and expand/collapse choices. id borrowed/copied (<=IPC_REQUEST_ID_BYTES).
 * TL_INVALID/LIMIT. */
tl_status popup_model_begin(tl_popup_model *model, const char *id);
/** Clear rows, items, selection, expand/collapse choices, search/status metadata
 * and the current request id. The window snapshot stays borrowed. All pending
 * responses become obsolete and borrowed row and item pointers expire.
 * No errors or ownership transfers; NULL allowed. */
void popup_model_clear(tl_popup_model *model);
/** Decode/apply current response; obsolete id returns TL_STATE without changes.
 * Final reordering preserves deliberately selected id AND position, including
 * one retained row outside top-k, and a selected child stays selected while
 * its parent lists it. TL_INVALID/LIMIT/STATE/IO; no borrowed input. */
tl_status popup_model_apply(tl_popup_model *model, const char *response, size_t length);
/** Decode validated response into caller rows, copying exact path bytes and
 * single-line labels. Uses model scratch, leaves selection untouched. Returns
 * TL_STATE for stale_result, TL_IO for remote error; TL_INVALID/LIMIT malformed.
 * search_id copied (may be empty); count zero on error. */
tl_status popup_model_decode(tl_popup_model *model, const char *response, size_t length,
                             tl_popup_row *rows, size_t capacity, size_t *count,
                             char search_id[IPC_HISTORY_ID_BYTES + 1]);
/** Move clamped selection across list items, keeping activation disabled until
 * current results. No errors. delta normally +/-1; no ownership changes. */
void popup_model_move(tl_popup_model *model, int delta);
/** Borrow the selected item's result row (the parent for children) until the
 * next mutation or destroy; NULL if not actionable. */
const tl_popup_row *popup_model_selected(const tl_popup_model *model);
/** Borrow the selected list item until the next mutation; NULL if not actionable. */
const tl_popup_item *popup_model_selected_item(const tl_popup_model *model);
/** Borrow an open-window snapshot until the next call or destroy (NULL for none)
 * and give each window to the displayed application with the strongest
 * windows_evidence; ties go to the higher-ranked row. Settings never own
 * windows. Call again after every windows_refresh of the same snapshot, before
 * reading items. Every application lists its windows wherever it ranks, unless
 * popup_model_collapse hid them. Keeps the selected item, or its result when
 * the item is gone. No errors or ownership transfer. */
void popup_model_set_windows(tl_popup_model *model, const tl_windows *windows);
/** Count list items: results and the children of expanded applications, in
 * display order (windows most recent first, then more windows, then new
 * window). Zero for NULL; no errors. */
size_t popup_model_item_count(const tl_popup_model *model);
/** Borrow the list item at index until the next mutation; NULL if absent. */
const tl_popup_item *popup_model_item(const tl_popup_model *model, size_t index);
/** List the selected application's windows, or every window from a more
 * windows item, until the next begin/clear. Selection stays on the result, or
 * moves to the first window the more item revealed, and counts as deliberate.
 * Returns whether the list changed; no errors. */
bool popup_model_expand(tl_popup_model *model);
/** Hide the children of the selected result, or of the selected child's parent,
 * until the next begin/clear, and select that result as deliberate. Returns
 * whether the list changed; no errors. */
bool popup_model_collapse(tl_popup_model *model);
/** Borrow row at index until next mutation; NULL for absent index. */
const tl_popup_row *popup_model_row(const tl_popup_model *model, size_t index);
/** Return how many leading bytes of row index's display parent must stay visible
 * to tell it apart from other rows of the same kind (file or folder) and name:
 * through the first folder where their parents differ. Zero for applications,
 * absent rows, or when no such row exists. No errors or ownership changes. */
size_t popup_model_distinct_prefix(const tl_popup_model *model, size_t index);
/** Read result count, selected item index, status and search id; borrowed
 * search id expires on apply. NULL-safe getters have zero/false/empty
 * defaults; no errors. */
size_t popup_model_count(const tl_popup_model *model);
size_t popup_model_position(const tl_popup_model *model);
bool popup_model_indexing(const tl_popup_model *model);
bool popup_model_degraded(const tl_popup_model *model);
const char *popup_model_search_id(const tl_popup_model *model);
#endif
