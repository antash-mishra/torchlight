/* Width-aware GTK path label retaining home/root, distinguishing and trailing folders. */
#ifndef TORCHLIGHT_GTK_PATH_LABEL_H
#define TORCHLIGHT_GTK_PATH_LABEL_H
#include <gtk/gtk.h>
#include <stddef.h>
/** Create a floating widget from borrowed valid UTF-8 display path. keep is the
 * byte length of a path prefix, ending before a '/', that should stay visible
 * when the path is shortened (0 for none, e.g. from popup_model_distinct_prefix);
 * out-of-range values are ignored. Caller parents/owns widget; label and tooltip
 * copy the full path, never an action target. Main thread only; GTK handles
 * allocation failure. */
GtkWidget *popup_path_label_new(const char *path, size_t keep);
#endif
