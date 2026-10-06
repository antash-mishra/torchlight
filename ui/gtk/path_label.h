/* Width-aware GTK path label retaining the home/root and useful trailing folders. */
#ifndef TORCHLIGHT_GTK_PATH_LABEL_H
#define TORCHLIGHT_GTK_PATH_LABEL_H
#include <gtk/gtk.h>
/** Create a floating widget from borrowed valid UTF-8 display path. Caller
 * parents/owns widget; label and tooltip copy the full path, never an action
 * target. Main thread only; GTK handles allocation failure. */
GtkWidget *popup_path_label_new(const char *path);
#endif
