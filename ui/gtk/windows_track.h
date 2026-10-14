/* Open windows that a compositor reports one event at a time, kept so that they can be
 * listed most recently used first (ADR 0052). Plain bookkeeping with no Wayland or GTK, so
 * the Wayland window source's ordering and naming rules are unit-testable. */
#ifndef TORCHLIGHT_GTK_WINDOWS_TRACK_H
#define TORCHLIGHT_GTK_WINDOWS_TRACK_H
#include "torchlight/windows.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct windows_track windows_track;
/** Create an empty track. TL_INVALID for a NULL out, TL_NOMEM; out NULL on error. */
tl_status windows_track_create(windows_track **out);
/** Destroy the track. The data pointers given to windows_track_add stay the caller's and
 * are not freed. NULL allowed. */
void windows_track_destroy(windows_track *track);
/** Add a window the compositor announced, with borrowed data the caller keeps (returned
 * by find and remove). It has no title, no app id and is not listed until marked ready.
 * Writes its id, never zero and never reused by this track. TL_INVALID for a NULL track or
 * id, TL_NOMEM; *id untouched on error. */
tl_status windows_track_add(windows_track *track, void *data, uint64_t *id);
/** Copy borrowed title as valid single-line UTF-8, cut at WINDOWS_TITLE_BYTES. Unknown ids
 * and NULL arguments are ignored; no errors. */
void windows_track_set_title(windows_track *track, uint64_t id, const char *title);
/** Copy borrowed app_id when it fits WINDOWS_CLASS_BYTES, else clear it (ids are compared
 * exactly, so a cut one would match the wrong application). Unknown ids and NULL
 * arguments are ignored; no errors. */
void windows_track_set_app_id(windows_track *track, uint64_t id, const char *app_id);
/** Record whether the window is the active one. Becoming active makes it the most recently
 * used window. Unknown ids are ignored; no errors. */
void windows_track_set_active(windows_track *track, uint64_t id, bool active);
/** Mark the window's first description complete, so that it is listed. Unknown ids are
 * ignored; no errors. */
void windows_track_set_ready(windows_track *track, uint64_t id);
/** Forget a closed window and return its data for the caller to free; NULL when id is
 * unknown. No errors. */
void *windows_track_remove(windows_track *track, uint64_t id);
/** Borrow the data of window id; NULL when unknown. No errors. */
void *windows_track_find(const windows_track *track, uint64_t id);
/** Data of the window at index (0 to count - 1, in no particular order), for visiting every
 * window; NULL past the end. Adding or removing windows changes the order. No errors. */
void *windows_track_data_at(const windows_track *track, size_t index);
/** Number of windows tracked, listed or not; zero for NULL. No errors. */
size_t windows_track_count(const windows_track *track);
/** Write up to capacity ready windows into out: those that have been active, most recently
 * active first, then the rest in the order they were added (compositors built on wlroots
 * announce the newest first). Windows whose app id equals borrowed skip_app_id
 * (may be NULL) are left out. Each window's handle is its id, and its app id also fills the
 * class and instance, which is what a Wayland compositor reports for both (an Xwayland
 * window's app id is its WM_CLASS class). Returns the number written; zero for NULL
 * arguments. No errors. */
size_t windows_track_list(const windows_track *track, const char *skip_app_id, tl_window *out,
                          size_t capacity);
#endif
