/* Private GTK widget: Tekst's idle strip, the index's state while the query is empty. */
#ifndef TORCHLIGHT_GTK_INDEX_STRIP_H
#define TORCHLIGHT_GTK_INDEX_STRIP_H
#include <gtk/gtk.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/** What the daemon's status reply says about the index. */
typedef struct {
    bool known;            /* false until the first status reply arrives */
    bool updating;         /* a scan or rebuild is running */
    bool degraded;         /* some folders are unavailable or not watched */
    size_t watches;        /* folders watched for changes */
    uint64_t last_scan_ms; /* duration of the last full scan */
} tl_popup_index;
/** Create a floating one-line strip of three facts, each behind a teletext colour
 * block; it says "Checking the index" until popup_index_strip_set gives it a known
 * state. Main thread; the caller parents it. */
GtkWidget *popup_index_strip_new(void);
/** Show the borrowed index state: whether it is up to date, updating or degraded,
 * then the folders watched and the last scan's time. Main thread; strip from
 * popup_index_strip_new. No errors. Call popup_index_strip_fit afterwards. */
void popup_index_strip_set(GtkWidget *strip, const tl_popup_index *index);
/** Keep the strip within width logical pixels by leaving out its least important
 * facts (the last scan, then the folders); the state always stays. Main thread;
 * strip from popup_index_strip_new. No errors. */
void popup_index_strip_fit(GtkWidget *strip, int width);
/** Borrow the text of fact 0..2 (empty when it has nothing to say) until the next
 * set, for tests. Main thread; strip from popup_index_strip_new; NULL for other
 * indexes. */
const char *popup_index_strip_text(GtkWidget *strip, size_t fact);
/** Whether fact 0..2 is shown after the last set and fit. Main thread. */
bool popup_index_strip_shown(GtkWidget *strip, size_t fact);
#endif
