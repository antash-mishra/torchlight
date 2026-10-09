/* Private launch task data, shared only by popup orchestration and actions. */
#ifndef TORCHLIGHT_GTK_ACTIONS_H
#define TORCHLIGHT_GTK_ACTIONS_H
#include "torchlight/popup.h"
#include <gio/gio.h>
struct launch {
    tl_popup_row row;
    tl_ipc_request event;
    GAppLaunchContext *context;
    /* reveal shows a file in its folder; new_window opens another application window. */
    bool reveal, new_window;
};
/** GTask worker: task_data is borrowed launch data until completion. Returns
 * accepted boolean through task; checks cancellation before launch. Performs
 * desktop validation, argv spawn and cancellable file-manager DBus outside main
 * context. A new window runs the entry's new-window (or new-empty-window)
 * desktop action, or a normal launch when it has neither. Cancellation prevents
 * an unaccepted fallback; accepted actions retain their result for asynchronous
 * history even if later canceled. */
void actions_worker(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel);
#endif
