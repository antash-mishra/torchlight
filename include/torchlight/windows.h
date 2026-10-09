/* Open top-level windows from a swappable window-system source, matched to applications. */
#ifndef TORCHLIGHT_WINDOWS_H
#define TORCHLIGHT_WINDOWS_H
#include "torchlight/common.h"
#include <stddef.h>
#include <stdint.h>
/* Windows kept per snapshot; the most recently used come first. */
#define WINDOWS_MAX 128
/* WM_CLASS parts and GTK application ids, NUL included; longer values are dropped. */
#define WINDOWS_CLASS_BYTES 256
/* Titles, NUL included; sources truncate at a UTF-8 character boundary. */
#define WINDOWS_TITLE_BYTES 512
/** How strongly a window belongs to an application; zero means it does not.
 * Higher values win when two applications claim the same window. */
typedef enum {
    WINDOWS_EVIDENCE_NONE,
    WINDOWS_EVIDENCE_DESKTOP_NAME, /* instance + ".desktop" is the desktop id */
    WINDOWS_EVIDENCE_APP_ID,       /* GTK application id + ".desktop" is the desktop id */
    WINDOWS_EVIDENCE_CLASS,        /* StartupWMClass equals the WM_CLASS class */
    WINDOWS_EVIDENCE_INSTANCE      /* StartupWMClass equals the WM_CLASS instance */
} tl_windows_evidence;
typedef struct {
    uint64_t handle; /* window-system id, never zero */
    char instance[WINDOWS_CLASS_BYTES], class_name[WINDOWS_CLASS_BYTES];
    char app_id[WINDOWS_CLASS_BYTES];
    char title[WINDOWS_TITLE_BYTES]; /* valid single-line UTF-8, may be empty */
} tl_window;
/** A window system. context is owned by the source and released by destroy.
 * list writes up to capacity windows, most recently used first, and the count;
 * it returns TL_IO when the system cannot be read. activate asks the window
 * manager to raise and focus handle, returning TL_STATE when it has closed.
 * Both run on the caller's thread and never block on anything but the window
 * system connection. */
typedef struct {
    void *context;
    tl_status (*list)(void *context, tl_window *out, size_t capacity, size_t *count);
    tl_status (*activate)(void *context, uint64_t handle);
    void (*destroy)(void *context);
} tl_window_source;
typedef struct tl_windows tl_windows;
/** Create an empty snapshot over source, which is copied. On success the
 * snapshot owns source->context and destroys it; on error the caller keeps it.
 * A NULL source never lists windows (window systems without support).
 * TL_INVALID for a NULL out or a source missing a function, TL_NOMEM;
 * out NULL on error. */
tl_status windows_create(const tl_window_source *source, tl_windows **out);
/** Destroy the snapshot and its source. Borrowed windows expire. NULL allowed. */
void windows_destroy(tl_windows *windows);
/** Replace the snapshot with the source's current windows. Windows with a zero
 * handle are dropped. On error the snapshot is left empty and the source's
 * status is returned. Borrowed windows and indices expire. TL_INVALID for NULL. */
tl_status windows_refresh(tl_windows *windows);
/** Number of windows in the snapshot; zero for NULL. No errors. */
size_t windows_count(const tl_windows *windows);
/** Borrow the window at index until the next refresh/destroy; NULL if absent. */
const tl_window *windows_get(const tl_windows *windows, size_t index);
/** Ask the window manager to show and focus the window at index. TL_INVALID for
 * NULL or an absent index; TL_STATE when the window has closed (refresh then);
 * other source errors unchanged. No ownership changes. */
tl_status windows_activate(tl_windows *windows, size_t index);
/** Rate how strongly window belongs to the application with borrowed desktop_id
 * (such as "google-chrome.desktop") and optional StartupWMClass wm_class (NULL
 * or empty when absent). Comparisons are exact, apart from the desktop-name
 * rule, which also tries the instance lowercased with spaces as hyphens. No
 * errors; NULL window or desktop_id gives WINDOWS_EVIDENCE_NONE. */
tl_windows_evidence windows_evidence(const tl_window *window, const char *desktop_id,
                                     const char *wm_class);
/** Copy borrowed title into out without a trailing " - Application" suffix
 * naming app_name (also an en or em dash). The suffix is removed when it equals
 * the name, is the name's first or last words, or ends with its first word,
 * ignoring ASCII case: "Docs - Google Chrome", "Page - Brave" for "Brave Web
 * Browser" and "Tab — Mozilla Firefox" for "Firefox Web Browser". A title that
 * would become empty is kept. Truncates at a UTF-8 boundary; out is always
 * NUL-terminated when capacity is nonzero. No errors or ownership changes. */
void windows_short_title(const char *title, const char *app_name, char *out, size_t capacity);
#endif
