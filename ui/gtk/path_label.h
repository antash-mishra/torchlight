/* Short, width-aware GTK folder label: home/root, a distinguishing folder and the last folders. */
#ifndef TORCHLIGHT_GTK_PATH_LABEL_H
#define TORCHLIGHT_GTK_PATH_LABEL_H
#include <gtk/gtk.h>
#include <stddef.h>
/** Create a floating widget from borrowed valid UTF-8 display path. It shows a
 * short form, right-aligned: the home folder as "~" and at most the last two
 * folders ("~/…/docs/modules"). keep is the byte length of a path prefix, ending
 * before a '/', whose last folder tells same-named rows apart and stays visible
 * ("~/…/27.0.12077973/…/include"; 0 for none, e.g. from
 * popup_model_distinct_prefix); out-of-range values are ignored. When even the
 * short form is too wide, earlier folders give way, then the last folder is cut
 * in the middle. Caller parents/owns widget; the tooltip and accessible label
 * copy the full path, never an action target. Main thread only; GTK handles
 * allocation failure. */
GtkWidget *popup_path_label_new(const char *path, size_t keep);
#endif
